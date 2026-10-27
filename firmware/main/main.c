/*
 * ESP32 butler: a light that shows whether the AI agents are working, asking a question, or idle.
 * Only starts things up and wires them together; each concern lives in its own component under components/.
 */
#include "bt_agents.h"
#include "bt_config.h"
#include "bt_led.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "serial_api.h"

static const char *TAG = "main";

void app_main(void)
{
    bt_led_start();   /* first, so booting shows; the butler runs on if it fails */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        bt_config_load();
    } else {
        /* Never erase: that would wipe the settings. Re-provision instead. */
        ESP_LOGW(TAG, "NVS unusable (%s), using the defaults: run ./bt.sh provision flash-nvs", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(bt_agents_start());
    ESP_ERROR_CHECK(serial_api_start());
    ESP_LOGI(TAG, "ready");
}
