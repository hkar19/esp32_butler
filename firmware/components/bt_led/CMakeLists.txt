idf_component_register(SRCS "bt_led.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_timer bt_config espressif__led_strip)
