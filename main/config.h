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

/* Quick-reply macro slots (Phase 3 chat): one-tap sends, edited on-device,
 * stored here. Empty string = unset. ESP_ERR_NVS_NVS_NOT_FOUND tolerated
 * by callers for never-saved slots. */
#define KAST_MACRO_COUNT      4
#define KAST_MACRO_TEXT_LEN   64

esp_err_t kast_config_load_macro(uint8_t slot, char *text, size_t text_len);
esp_err_t kast_config_save_macro(uint8_t slot, const char *text);

/* Crash forensics for the field: this boot's reset reason (i.e. why the
 * previous boot died — panic, watchdog, brownout...), kept in RAM and
 * handed to the relay as a dev.hello frame on connect — serial logs
 * scroll away before anyone can read them remotely, but the relay log
 * waits. The code is also persisted in NVS as a breadcrumb. Call
 * kast_config_note_reset() FIRST in app_main. */
void kast_config_note_reset(int reset_code);
const char *kast_config_last_reset_str(void);   /* this boot's */

#ifdef __cplusplus
}
#endif
