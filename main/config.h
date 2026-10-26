/*
 * config.h — KAST NVS configuration layer.
 *
 * Everything the user can change on-device lives here: Wi-Fi credentials
 * (D-4 — entered on the settings screen, never in code) and the PC relay
 * endpoint (Phase 2 WebSocket client). Callers must tolerate
 * ESP_ERR_NVS_NOT_FOUND: a fresh board has nothing saved yet.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load Wi-Fi creds from NVS. ESP_ERR_NVS_NOT_FOUND when never saved —
 * callers fall back to the bench header (wifi_config.h). */
esp_err_t kast_config_load_wifi(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len);
esp_err_t kast_config_save_wifi(const char *ssid, const char *pass);

/* Relay endpoint for the WebSocket client. Host empty ("") when unset. */
esp_err_t kast_config_load_relay(char *host, size_t host_len, uint16_t *port);
esp_err_t kast_config_save_relay(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif
