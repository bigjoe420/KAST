/*
 * config.c — KAST NVS configuration layer (see config.h).
 *
 * One namespace ("kast"), string keys. No caching: reads are rare
 * (boot, settings save) and NVS handles wear leveling itself.
 */
#include <string.h>
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
