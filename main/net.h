/*
 * net.h — KAST Wi-Fi station interface.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the Wi-Fi station: init, load creds from wifi_config.h, connect,
 * and keep reconnecting with backoff until an IP is held.
 * Updates the UI status bar via kast_ui_net_status().
 */
esp_err_t kast_net_init(void);

#ifdef __cplusplus
}
#endif
