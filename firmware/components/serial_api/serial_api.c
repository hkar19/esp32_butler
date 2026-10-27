#include "serial_api.h"

#include <stdio.h>
#include <string.h>
#include "bt_agents.h"
#include "bt_led.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "serial_api";

#define UART_PORT        CONFIG_ESP_CONSOLE_UART_NUM
#define REQUEST_MAX      512
#define HOST_TIMEOUT_S   45

static int64_t s_last_request_us;
static bool s_host_up;

static cJSON *error(const char *code, const char *message)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(reply, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return reply;
}

static cJSON *ok(void)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddTrueToObject(reply, "ok");
    return reply;
}

static cJSON *op_state(const cJSON *req)
{
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(req, "session"));
    const char *state_text = cJSON_GetStringValue(cJSON_GetObjectItem(req, "state"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
    const cJSON *t = cJSON_GetObjectItem(req, "t");
    bt_agent_state_t state;
    if (session == NULL || session[0] == '\0' || strlen(session) >= BT_AGENTS_ID_MAX) {
        return error("bad_session", "session must be a string of 1-63 bytes");
    }
    if (state_text == NULL || bt_agents_state_parse(state_text, &state) != ESP_OK) {
        return error("bad_state", "state must be working, question, idle or end");
    }
    bt_agents_report(session, name, state, cJSON_IsNumber(t) ? (int64_t)cJSON_GetNumberValue(t) : 0);
    cJSON *reply = ok();
    cJSON_AddStringToObject(reply, "led", bt_led_state_name());
    return reply;
}

static cJSON *op_get(void)
{
    static bt_agent_info_t agents[BT_AGENTS_MAX];   /* only the serial task calls this */
    size_t n = bt_agents_list(agents, BT_AGENTS_MAX);

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "led", bt_led_state_name());
    cJSON *list = cJSON_AddArrayToObject(reply, "agents");
    for (size_t i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "session", agents[i].id);
        cJSON_AddStringToObject(a, "name", agents[i].name);
        cJSON_AddStringToObject(a, "state", bt_agents_state_name(agents[i].state));
        cJSON_AddNumberToObject(a, "silent_s", agents[i].silent_s);
        cJSON_AddItemToArray(list, a);
    }
    return reply;
}

static cJSON *op_led_test(const cJSON *req)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "state"));
    const cJSON *seconds_item = cJSON_GetObjectItem(req, "seconds");
    int seconds = cJSON_IsNumber(seconds_item) ? (int)cJSON_GetNumberValue(seconds_item) : 5;
    if (seconds < 1 || seconds > BT_LED_TEST_MAX_S) {
        return error("bad_seconds", "seconds must be 1-30");
    }
    const char *looks;
    if (name == NULL || bt_led_test(name, seconds, &looks) != ESP_OK) {
        return error("bad_state", "state must be one of: " BT_LED_TEST_NAMES);
    }
    cJSON *reply = ok();
    cJSON_AddStringToObject(reply, "state", name);
    cJSON_AddNumberToObject(reply, "seconds", seconds);
    cJSON_AddStringToObject(reply, "looks", looks);
    return reply;
}

static cJSON *op_health(void)
{
    cJSON *reply = ok();
    cJSON_AddNumberToObject(reply, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(reply, "fw", esp_app_get_description()->version);
    return reply;
}

/* Sends "@bt <json>" as one line, and frees reply */
static void send_reply(cJSON *reply)
{
    char *text = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    printf("@bt %s\n", text != NULL ? text : "{\"error\":{\"code\":\"no_memory\",\"message\":\"out of memory\"}}");
    fflush(stdout);
    cJSON_free(text);
}

static void host_heard(void)
{
    s_last_request_us = esp_timer_get_time();
    if (!s_host_up) {
        s_host_up = true;
        bt_led_set(BT_LED_NO_HOST, false);
        ESP_LOGI(TAG, "the host is talking");
    }
}

static void handle_line(const char *line)
{
    cJSON *req = cJSON_Parse(line);
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(req, "op"));
    cJSON *reply;
    if (op == NULL) {
        reply = error("bad_request", "a request is one line holding a JSON object with \"op\"");
    } else {
        host_heard();
        if (strcmp(op, "state") == 0) {
            reply = op_state(req);
        } else if (strcmp(op, "get") == 0) {
            reply = op_get();
        } else if (strcmp(op, "clear") == 0) {
            bt_agents_clear();
            reply = ok();
        } else if (strcmp(op, "led_test") == 0) {
            reply = op_led_test(req);
        } else if (strcmp(op, "health") == 0) {
            reply = op_health();
        } else {
            reply = error("bad_op", "op must be state, get, clear, led_test or health");
        }
    }
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(reply, "id", cJSON_GetNumberValue(id));
    }
    cJSON_Delete(req);
    send_reply(reply);
}

static void serial_task(void *arg)
{
    static char line[REQUEST_MAX + 1];
    size_t len = 0;
    bool too_long = false;
    for (;;) {
        /* uart_read_bytes waits until it has every byte asked for, so wait for one, then take what else is there */
        uint8_t buf[64];
        int n = uart_read_bytes(UART_PORT, buf, 1, pdMS_TO_TICKS(1000));
        size_t more = 0;
        if (n == 1 && uart_get_buffered_data_len(UART_PORT, &more) == ESP_OK && more > 0) {
            int got = uart_read_bytes(UART_PORT, buf + 1, more < sizeof(buf) - 1 ? more : sizeof(buf) - 1, 0);
            n += got > 0 ? got : 0;
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (too_long) {
                    ESP_LOGW(TAG, "a line over %d bytes, dropped", REQUEST_MAX);
                } else if (len > 0 && line[0] == '{') {
                    line[len] = '\0';
                    handle_line(line);
                }
                len = 0;
                too_long = false;
            } else if (len < REQUEST_MAX) {
                line[len++] = c;
            } else {
                too_long = true;
            }
        }
        if (s_host_up && esp_timer_get_time() - s_last_request_us > HOST_TIMEOUT_S * 1000000LL) {
            s_host_up = false;
            bt_led_set(BT_LED_NO_HOST, true);
            ESP_LOGW(TAG, "no word from the host for %d s", HOST_TIMEOUT_S);
        }
    }
}

esp_err_t serial_api_start(void)
{
    bt_led_set(BT_LED_NO_HOST, true);   /* until the first request */
    esp_err_t err = uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    uart_vfs_dev_use_driver(UART_PORT);   /* printf and the log now go through the driver too, so they don't clash */
    if (xTaskCreate(serial_task, "serial_api", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    printf("@bt {\"hello\":true,\"fw\":\"%s\"}\n", esp_app_get_description()->version);
    fflush(stdout);
    ESP_LOGI(TAG, "listening on UART%d", UART_PORT);
    return ESP_OK;
}
