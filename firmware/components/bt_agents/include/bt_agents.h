/*
 * The agents (Claude Code sessions, or anything else that reports) and what each one is doing.
 * Every change updates the LED with the most urgent state: question > working > idle > none.
 *
 * Reports from one session can arrive out of order (the hook sends them in the background), so each carries
 * the sender's clock in ms; a report older than the last one seen for that session is dropped.
 * A working session silent for working_stale_s shows as idle; any session silent for session_ttl_s is forgotten.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define BT_AGENTS_MAX        16
#define BT_AGENTS_ID_MAX     64
#define BT_AGENTS_NAME_MAX   32

typedef enum {
    BT_AGENT_IDLE = 0,
    BT_AGENT_WORKING,
    BT_AGENT_QUESTION,
    BT_AGENT_ENDED,          /* only kept for a minute, so late reports from an ended session are dropped */
} bt_agent_state_t;

typedef struct {
    char id[BT_AGENTS_ID_MAX];
    char name[BT_AGENTS_NAME_MAX];   /* e.g. the project folder; may be "" */
    bt_agent_state_t state;          /* as shown: a stale "working" is already IDLE here */
    uint32_t silent_s;               /* since the last report */
} bt_agent_info_t;

esp_err_t bt_agents_start(void);

/* t_ms: the sender's clock, 0 if it has none (then the report is never dropped as stale).
 * name may be NULL or "" to keep the name already known. ESP_ERR_INVALID_ARG: empty id or bad state. */
esp_err_t bt_agents_report(const char *id, const char *name, bt_agent_state_t state, int64_t t_ms);

/* Forgets every agent. */
void bt_agents_clear(void);

/* The live agents (not ended), at most max. Returns how many were written. */
size_t bt_agents_list(bt_agent_info_t *out, size_t max);

const char *bt_agents_state_name(bt_agent_state_t state);

/* "idle", "working", "question" or "end" to its state. ESP_ERR_NOT_FOUND for anything else. */
esp_err_t bt_agents_state_parse(const char *text, bt_agent_state_t *out);
