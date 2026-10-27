#include "bt_config.h"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "bt_config";

static bt_config_t s_cfg = {
    .led_brightness_pct = CONFIG_BT_LED_BRIGHTNESS_PCT,
    .working_stale_s = CONFIG_BT_WORKING_STALE_S,
    .session_ttl_s = CONFIG_BT_SESSION_TTL_S,
};

/* tools/provision.py checks the ranges; an out-of-range value here means a hand-made image, so keep the default */
static void read_tunables(nvs_handle_t h)
{
    const struct {
        const char *key;
        uint32_t *value;
        uint32_t min, max;
    } items[] = {
        { "led_bright",  &s_cfg.led_brightness_pct, 1,   100 },
        { "work_stale",  &s_cfg.working_stale_s,    60,  86400 },
        { "session_ttl", &s_cfg.session_ttl_s,      300, 604800 },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        uint32_t v;
        if (nvs_get_u32(h, items[i].key, &v) != ESP_OK) {
            continue;
        }
        if (v < items[i].min || v > items[i].max) {
            ESP_LOGW(TAG, "\"%s\"=%u is out of range, keeping %u", items[i].key, (unsigned)v, (unsigned)*items[i].value);
            continue;
        }
        *items[i].value = v;
    }
}

void bt_config_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("bt", NVS_READONLY, &h);
    if (err == ESP_OK) {
        read_tunables(h);
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "no settings in NVS (%s), using the defaults", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "led=%u%% working_stale=%us session_ttl=%us", (unsigned)s_cfg.led_brightness_pct,
             (unsigned)s_cfg.working_stale_s, (unsigned)s_cfg.session_ttl_s);
}

const bt_config_t *bt_config(void)
{
    return &s_cfg;
}
