#include "bt_agents.h"

#include <string.h>
#include "bt_config.h"
#include "bt_led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bt_agents";

#define SWEEP_INTERVAL_US   (5 * 1000000LL)
#define ENDED_KEEP_S        60

typedef struct {
    bool used;
    char id[BT_AGENTS_ID_MAX];
    char name[BT_AGENTS_NAME_MAX];
    bt_agent_state_t state;
    int64_t last_t_ms;           /* the sender's clock on the newest report */
    int64_t seen_us;             /* our clock when it arrived */
} slot_t;

static slot_t s_slots[BT_AGENTS_MAX];
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_sweep;

static const char *const NAMES[] = { "idle", "working", "question", "end" };

const char *bt_agents_state_name(bt_agent_state_t state)
{
    return state <= BT_AGENT_ENDED ? NAMES[state] : "?";
}

esp_err_t bt_agents_state_parse(const char *text, bt_agent_state_t *out)
{
    for (int i = 0; i <= BT_AGENT_ENDED; i++) {
        if (strcmp(text, NAMES[i]) == 0) {
            *out = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static uint32_t silent_s(const slot_t *s, int64_t now_us)
{
    return (uint32_t)((now_us - s->seen_us) / 1000000);
}

/* The state to show, with a stale "working" turned into idle */
static bt_agent_state_t shown_state(const slot_t *s, int64_t now_us)
{
    if (s->state == BT_AGENT_WORKING && silent_s(s, now_us) >= bt_config()->working_stale_s) {
        return BT_AGENT_IDLE;
    }
    return s->state;
}

/* With the lock held: drops expired slots and puts the most urgent state on the LED */
static void refresh_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    bt_led_agents_t led = BT_LED_AGENTS_NONE;
    for (int i = 0; i < BT_AGENTS_MAX; i++) {
        slot_t *s = &s_slots[i];
        if (!s->used) {
            continue;
        }
        uint32_t silent = silent_s(s, now_us);
        if ((s->state == BT_AGENT_ENDED && silent >= ENDED_KEEP_S) || silent >= bt_config()->session_ttl_s) {
            if (s->state != BT_AGENT_ENDED) {
                ESP_LOGI(TAG, "%s (%s): silent for %u s, forgotten", s->id, s->name, (unsigned)silent);
            }
            s->used = false;
            continue;
        }
        bt_led_agents_t level;
        switch (shown_state(s, now_us)) {
        case BT_AGENT_QUESTION: level = BT_LED_AGENTS_QUESTION; break;
        case BT_AGENT_WORKING:  level = BT_LED_AGENTS_WORKING; break;
        case BT_AGENT_IDLE:     level = BT_LED_AGENTS_IDLE; break;
        default:                level = BT_LED_AGENTS_NONE; break;
        }
        if (level > led) {
            led = level;
        }
    }
    bt_led_set_agents(led);
}

static void sweep_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_locked();
    xSemaphoreGive(s_lock);
}

/* With the lock held: the slot for id, or a free one, or the one silent the longest (ended ones first) */
static slot_t *slot_for_locked(const char *id)
{
    slot_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < BT_AGENTS_MAX; i++) {
        slot_t *s = &s_slots[i];
        if (s->used && strcmp(s->id, id) == 0) {
            return s;
        }
        if (!s->used) {
            free_slot = free_slot ? free_slot : s;
        } else if (oldest == NULL || (s->state == BT_AGENT_ENDED) > (oldest->state == BT_AGENT_ENDED) ||
                   ((s->state == BT_AGENT_ENDED) == (oldest->state == BT_AGENT_ENDED) && s->seen_us < oldest->seen_us)) {
            oldest = s;
        }
    }
    slot_t *s = free_slot ? free_slot : oldest;
    if (!free_slot) {
        ESP_LOGW(TAG, "table full: dropping %s to make room", oldest->id);
    }
    memset(s, 0, sizeof(*s));
    return s;
}

esp_err_t bt_agents_report(const char *id, const char *name, bt_agent_state_t state, int64_t t_ms)
{
    if (id == NULL || id[0] == '\0' || state > BT_AGENT_ENDED) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = slot_for_locked(id);
    if (s->used && t_ms != 0 && t_ms < s->last_t_ms) {
        ESP_LOGD(TAG, "%s: %s arrived late, dropped", id, bt_agents_state_name(state));
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    bool is_new = !s->used;
    if (is_new) {
        s->used = true;
        strlcpy(s->id, id, sizeof(s->id));
    }
    if (name != NULL && name[0] != '\0') {
        strlcpy(s->name, name, sizeof(s->name));
    }
    if (is_new || s->state != state) {
        ESP_LOGI(TAG, "%s (%s): %s", s->id, s->name, bt_agents_state_name(state));
    }
    s->state = state;
    s->last_t_ms = t_ms;
    s->seen_us = esp_timer_get_time();
    refresh_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void bt_agents_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_slots, 0, sizeof(s_slots));
    refresh_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "cleared");
}

size_t bt_agents_list(bt_agent_info_t *out, size_t max)
{
    size_t n = 0;
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BT_AGENTS_MAX && n < max; i++) {
        const slot_t *s = &s_slots[i];
        if (!s->used || s->state == BT_AGENT_ENDED) {
            continue;
        }
        strlcpy(out[n].id, s->id, sizeof(out[n].id));
        strlcpy(out[n].name, s->name, sizeof(out[n].name));
        out[n].state = shown_state(s, now_us);
        out[n].silent_s = silent_s(s, now_us);
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t bt_agents_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = { .callback = sweep_cb, .name = "agents_sweep" };
    esp_err_t err = esp_timer_create(&args, &s_sweep);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_sweep, SWEEP_INTERVAL_US);
}
