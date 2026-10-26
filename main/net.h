/*
 * net.h — KAST Wi-Fi station interface.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the Wi-Fi station: init, load creds (NVS first, bench header
 * fallback), connect, and keep reconnecting with backoff until an IP is
 * held. Updates the status-bar Wi-Fi link word via kast_ui_link_status().
 */
esp_err_t kast_net_init(void);

/* Settings save path: apply new creds and reconnect, no reboot.
 * Safe from the LVGL task (Wi-Fi API is task-safe). No-op before init.
 */
void kast_net_apply_creds(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
