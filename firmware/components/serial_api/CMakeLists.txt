idf_component_register(SRCS "serial_api.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_driver_uart vfs esp_timer esp_app_format bt_agents bt_led espressif__cjson)
