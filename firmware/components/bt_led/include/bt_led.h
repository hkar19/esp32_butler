/*
 * The status LED: the DevKit's WS2812 on GPIO8.
 *
 * Board problems (set with bt_led_set) outrank the agents' state (set with bt_led_set_agents).
 * Every state changes over time, so an LED that stays lit or dark without changing means frozen firmware.
 *
 *   Rank  State             Colour   Pattern
 *   1     (booting)         white    solid, until the host first talks, at most BT_LED_BOOT_MS
 *   2     NO_HOST           red      double blink every 2 s   no request over USB serial lately (serial_api)
 *   3     question          amber    fast blink, 4 Hz        an agent waits for your answer or permission
 *   4     working           blue     breathing, 2 s           an agent is busy
 *   5     idle              green    short blip every 2 s     every agent is done and waits for a new prompt
 *   6     no agents         white    short dim blip every 3 s
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    BT_LED_NO_HOST = 0,
    BT_LED_COND_COUNT,
} bt_led_cond_t;

typedef enum {
    BT_LED_AGENTS_NONE = 0,
    BT_LED_AGENTS_IDLE,
    BT_LED_AGENTS_WORKING,
    BT_LED_AGENTS_QUESTION,
} bt_led_agents_t;

/* Starts the LED task. Conditions set before this are kept. A failure leaves the butler running without the LED. */
esp_err_t bt_led_start(void);

void bt_led_set(bt_led_cond_t cond, bool active);

/* The most urgent state across all agents (bt_agents). */
void bt_led_set_agents(bt_led_agents_t agents);

/* What the LED is showing: "booting", "no_host", "question", "working", "idle" or "no_agents". */
const char *bt_led_state_name(void);

#define BT_LED_TEST_NAMES "booting, no_host, question, working, idle, no_agents, off"
#define BT_LED_TEST_MAX_S 30

/*
 * Shows one state's colour and pattern for a while, whatever the real state is, so every state can be checked
 * by eye ({"op":"led_test"}, ./bt.sh led-test). name is a bt_led_state_name() value, or "off" to stop a running
 * test. *looks gets a description such as "amber, fast blink (4 per second)". ESP_ERR_NOT_FOUND: unknown name.
 */
esp_err_t bt_led_test(const char *name, unsigned seconds, const char **looks);
