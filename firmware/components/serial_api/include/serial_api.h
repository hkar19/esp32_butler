/*
 * The butler's API over USB serial: the console UART, at 115200 baud, shared with the log. See README.md, "Serial API".
 *
 * A request is one line holding a JSON object with "op"; lines that don't start with '{' are ignored. Each request
 * gets one answer line, "@bt " + JSON, which repeats the request's "id" if it had one. Log lines never start with "@bt".
 *
 *   {"op":"state","session":"..","state":"working|question|idle|end","name":"..","t":<ms>}   {"ok":true,"led":".."}
 *   {"op":"get"}                            {"led":"..","agents":[{"session","name","state","silent_s"}]}
 *   {"op":"clear"}                          forget every agent
 *   {"op":"led_test","state":"X","seconds":N}  show one LED state for N seconds (state "off" stops)
 *   {"op":"health"}                         {"ok":true,"uptime_s":..,"fw":".."}
 *   errors                                  {"error":{"code":"..","message":".."}}
 *
 * At boot the board says {"hello":true,"fw":".."} (after "@bt "), so the bridge knows to send the sessions again.
 * No request for HOST_TIMEOUT_S lights the no_host state (the bridge sends health every 15 s).
 */
#pragma once

#include "esp_err.h"

esp_err_t serial_api_start(void);
