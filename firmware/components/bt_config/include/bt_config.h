/*
 * The board's settings, from NVS namespace "bt" (written by ./bt.sh provision flash-nvs).
 * All optional: anything missing keeps its Kconfig default.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t led_brightness_pct;
    uint32_t working_stale_s;
    uint32_t session_ttl_s;
} bt_config_t;

/* Reads the settings over the defaults. Needs NVS initialised. */
void bt_config_load(void);

/* The settings: the defaults until bt_config_load(). */
const bt_config_t *bt_config(void);
