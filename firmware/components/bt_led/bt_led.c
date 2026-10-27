#include "bt_led.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "bt_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "bt_led";

#define TICK_MS          30
#define BT_LED_BOOT_MS   10000

typedef struct {
    uint8_t r, g, b;
    uint16_t period_ms;          /* 0 = solid */
    uint8_t blinks;              /* blinks at the start of each period; 0 = breathe over the whole period */
    uint16_t on_ms;              /* each blink's on time; the gap between blinks is the same */
    const char *name;
    const char *looks;           /* what you see, for the LED test */
} pattern_t;

#define RED      255, 0, 0
#define AMBER    255, 90, 0
#define GREEN    0, 255, 0
#define BLUE     0, 0, 255
#define WHITE    255, 255, 255
#define DIM      60, 60, 60

static const pattern_t P_BOOT      = { WHITE, 0, 0, 0, "booting", "white, solid" };
static const pattern_t P_NO_HOST   = { RED, 2000, 2, 150, "no_host", "red, double blink every 2 s" };
static const pattern_t P_QUESTION  = { AMBER, 250, 1, 125, "question", "amber, fast blink (4 per second)" };
static const pattern_t P_WORKING   = { BLUE, 2000, 0, 0, "working", "blue, breathing (every 2 s)" };
static const pattern_t P_IDLE      = { GREEN, 2000, 1, 100, "idle", "green, short blip every 2 s" };
static const pattern_t P_NO_AGENTS = { DIM, 3000, 1, 60, "no_agents", "dim white, short blip every 3 s" };

/* Indexed by bt_led_agents_t */
static const pattern_t *const AGENT_PATTERNS[] = { &P_NO_AGENTS, &P_IDLE, &P_WORKING, &P_QUESTION };

static const pattern_t *const ALL[] = {
    &P_BOOT, &P_NO_HOST, &P_QUESTION, &P_WORKING, &P_IDLE, &P_NO_AGENTS,
};

static atomic_uint s_conds;
static atomic_int s_agents;
static atomic_bool s_host_seen;              /* NO_HOST has been cleared at least once since boot */
static led_strip_handle_t s_strip;
static const pattern_t *_Atomic s_test;      /* the pattern an LED test is showing, or NULL */
static _Atomic int64_t s_test_until_us;

void bt_led_set(bt_led_cond_t cond, bool active)
{
    if (cond >= BT_LED_COND_COUNT) {
        return;
    }
    if (active) {
        atomic_fetch_or(&s_conds, 1u << cond);
    } else {
        atomic_fetch_and(&s_conds, ~(1u << cond));
        if (cond == BT_LED_NO_HOST) {
            atomic_store(&s_host_seen, true);
        }
    }
}

void bt_led_set_agents(bt_led_agents_t agents)
{
    if (agents <= BT_LED_AGENTS_QUESTION) {
        atomic_store(&s_agents, agents);
    }
}

static const pattern_t *current(void)
{
    unsigned conds = atomic_load(&s_conds);
    if (!atomic_load(&s_host_seen) && esp_timer_get_time() < BT_LED_BOOT_MS * 1000LL) {
        return &P_BOOT;
    }
    if (conds & (1u << BT_LED_NO_HOST)) {
        return &P_NO_HOST;
    }
    return AGENT_PATTERNS[atomic_load(&s_agents)];
}

const char *bt_led_state_name(void)
{
    return current()->name;
}

esp_err_t bt_led_test(const char *name, unsigned seconds, const char **looks)
{
    const pattern_t *p = NULL;
    if (strcmp(name, "off") != 0) {
        for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
            if (strcmp(name, ALL[i]->name) == 0) {
                p = ALL[i];
            }
        }
        if (p == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    atomic_store(&s_test_until_us, esp_timer_get_time() + (int64_t)seconds * 1000000);
    atomic_store(&s_test, p);
    if (p != NULL) {
        ESP_LOGW(TAG, "LED test: \"%s\" for %u s -> %s", name, seconds, p->looks);
    } else {
        ESP_LOGW(TAG, "LED test: off, showing the real state again (%s)", bt_led_state_name());
    }
    if (looks != NULL) {
        *looks = p != NULL ? p->looks : "the real state";
    }
    return ESP_OK;
}

/* How lit the pattern is at this moment, 0-100 */
static uint32_t pattern_level(const pattern_t *p, int64_t now_ms)
{
    if (p->period_ms == 0) {
        return 100;
    }
    uint32_t phase = now_ms % p->period_ms;
    if (p->blinks == 0) {
        /* Breathe: a triangle from 5% up to 100% and back, never fully dark */
        uint32_t half = p->period_ms / 2;
        uint32_t up = phase < half ? phase : p->period_ms - phase;
        return 5 + 95 * up / half;
    }
    uint32_t slot = phase / p->on_ms;       /* on, off, on, off, ... from the start of the period */
    return slot < 2u * p->blinks && slot % 2 == 0 ? 100 : 0;
}

static void led_task(void *arg)
{
    uint32_t shown = UINT32_MAX;             /* packed RGB last sent, so the LED is only written on a change */
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        const pattern_t *p = current();
        const pattern_t *test = atomic_load(&s_test);
        if (test != NULL) {
            if (now_us < atomic_load(&s_test_until_us)) {
                p = test;
            } else {
                atomic_store(&s_test, NULL);
                ESP_LOGW(TAG, "LED test finished, showing the real state again (%s)", bt_led_state_name());
            }
        }

        uint32_t scale = pattern_level(p, now_us / 1000) * bt_config()->led_brightness_pct;   /* out of 100 * 100 */
        uint8_t r = p->r * scale / 10000;
        uint8_t g = p->g * scale / 10000;
        uint8_t b = p->b * scale / 10000;
        uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        if (rgb != shown) {
            led_strip_set_pixel(s_strip, 0, r, g, b);
            led_strip_refresh(s_strip);
            shown = rgb;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t bt_led_start(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = CONFIG_BT_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
#if CONFIG_BT_LED_ORDER_RGB
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
#else
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
#endif
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(led_task, "led", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
