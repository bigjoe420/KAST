/*
 * net.c — KAST Wi-Fi station bring-up.
 *
 * Creds come from wifi_config.h — a LOCAL, GITIGNORED header that never
 * enters version control. The product path is on-device entry into NVS;
 * this header is the bench bridge so bring-up never hardcodes secrets in
 * committed source.
 *
 * Policy: reconnect forever with backoff, log every transition, and push
 * status to the UI status bar.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"

#include "net.h"
#include "ui.h"
#include "wifi_config.h"

static const char *TAG = "net";

static uint8_t s_retry = 0;
#define KAST_NET_MAX_RETRY 10

static void kast_net_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "sta started, connecting to '%s'", KAST_WIFI_SSID);
        kast_ui_net_status(false, "WIFI ...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        s_retry++;
        if (s_retry > KAST_NET_MAX_RETRY) {
            ESP_LOGE(TAG, "give up after %d retries — rebooting to retry clean", KAST_NET_MAX_RETRY);
            kast_ui_net_status(false, "WIFI FAIL");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
        ESP_LOGW(TAG, "disconnected (retry %d/%d) reason=%d rssi=%d",
                 s_retry, KAST_NET_MAX_RETRY, disc->reason, disc->rssi);
        kast_ui_net_status(false, "WIFI RETRY");
        uint32_t backoff_ms = 1000u * s_retry;
        if (backoff_ms > 15000u) {
            backoff_ms = 15000u;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_retry = 0;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        kast_ui_net_status(true, NULL); /* UI shows link state + IP in log */
    }
}

esp_err_t kast_net_init(void)
{
    ESP_RETURN_ON_FALSE(strlen(KAST_WIFI_SSID) > 0, ESP_ERR_INVALID_STATE,
                        TAG, "no Wi-Fi creds — fill main/wifi_config.h (local, gitignored)");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &kast_net_handler, NULL),
                        TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &kast_net_handler, NULL),
                        TAG, "ip event register failed");

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, KAST_WIFI_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, KAST_WIFI_PASSWORD, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "Wi-Fi station up (ssid '%s')", KAST_WIFI_SSID);
    return ESP_OK;
}
