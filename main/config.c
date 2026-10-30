/*
 * config.c — KAST NVS configuration layer (see config.h).
 *
 * One namespace ("kast"), string keys. No caching: reads are rare
 * (boot, settings save) and NVS handles wear leveling itself.
 */
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "config.h"

#define KAST_CFG_NAMESPACE "kast"

static esp_err_t kast_config_load_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KAST_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, key, buf, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t kast_config_save_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KAST_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t kast_config_load_wifi(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len)
{
    esp_err_t err = kast_config_load_str("wifi_ssid", ssid, ssid_len);
    if (err != ESP_OK) {
        return err;
    }
    return kast_config_load_str("wifi_pass", pass, pass_len);
}

esp_err_t kast_config_save_wifi(const char *ssid, const char *pass)
{
    esp_err_t err = kast_config_save_str("wifi_ssid", ssid);
    if (err != ESP_OK) {
        return err;
    }
    return kast_config_save_str("wifi_pass", pass);
}

esp_err_t kast_config_load_relay(char *host, size_t host_len, uint16_t *port)
{
    uint16_t stored_port = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KAST_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, "relay_host", host, &host_len);
    if (err == ESP_OK) {
        err = nvs_get_u16(handle, "relay_port", &stored_port);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        *port = stored_port;
    }
    return err;
}

esp_err_t kast_config_save_relay(const char *host, uint16_t port)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KAST_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "relay_host", host);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "relay_port", port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t kast_config_load_macro(uint8_t slot, char *text, size_t text_len)
{
    if (slot >= KAST_MACRO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    snprintf(key, sizeof(key), "macro%u", (unsigned)slot);
    return kast_config_load_str(key, text, text_len);
}

esp_err_t kast_config_save_macro(uint8_t slot, const char *text)
{
    if (slot >= KAST_MACRO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    snprintf(key, sizeof(key), "macro%u", (unsigned)slot);
    return kast_config_save_str(key, text);
}

/* Crash forensics (see config.h): remember WHY this boot happened.
 * esp_reset_reason() already names the previous boot's killer (panic,
 * watchdog, brownout...) and stays valid all boot, so the relay report
 * is just that mapping kept in RAM. We also persist the code in NVS as
 * a breadcrumb a later tool can read even if the boot dies before the
 * relay link comes up. Call FIRST in app_main. */
static char s_last_reset[24] = "unknown";

static const char *cfg_reset_reason_str(int r)
{
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external pin";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep sleep";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu lockup";
    default:                 return "unknown";
    }
}

void kast_config_note_reset(int reset_code)
{
    snprintf(s_last_reset, sizeof(s_last_reset), "%s",
             cfg_reset_reason_str(reset_code));
    nvs_handle_t handle;
    if (nvs_open(KAST_CFG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "last_reset", (uint8_t)reset_code);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

const char *kast_config_last_reset_str(void)
{
    return s_last_reset;
}
