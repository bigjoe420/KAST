/*
 * relay.h — KAST PC-relay WebSocket client (K-5).
 *
 * Owns the device-side link to the PC relay server (Phase 3 wires the
 * actual chat/OBS protocol on top). Responsibilities: connect when
 * Wi-Fi holds an IP, auto-reconnect with backoff, websocket ping to
 * kill half-open sockets, RELAY status-bar word.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the relay link manager. Loads the endpoint from NVS (set on the
 * settings screen); with no endpoint the link stays down with a "SET"
 * hint in the status bar. Call after kast_net_init(); connects on
 * IP_EVENT_STA_GOT_IP, tears down while Wi-Fi is down.
 */
esp_err_t kast_relay_init(void);

/* Settings save path: reload the endpoint from NVS and restart the link
 * if it is running. Safe to call from the LVGL task.
 */
esp_err_t kast_relay_apply(void);

#ifdef __cplusplus
}
#endif
