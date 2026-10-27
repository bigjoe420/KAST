/*
 * net.c — KAST Wi-Fi station bring-up.
 *
 * Creds: NVS first (entered on-device via the settings screen, D-4),
 * falling back to wifi_config.h — a LOCAL, GITIGNORED bench header that
 * never enters version control (builds without it get empty strings and
 * rely on NVS creds alone). With no creds at all the station soft-fails
 * and the UI keeps running.
 *
 * Policy: reconnect forever with capped backoff, log every transition,
 * and push status to the status-bar Wi-Fi link word. A wrong or
 * unreachable network NEVER reboots the box — slow-retry every 30 s
 * instead (a reboot loop made on-device settings editing impossible).
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
#include "config.h"

/* Bench creds header: local + gitignored (D-4). Builds without it (CI,
 * fresh clones) get empty strings — NVS-entered creds are the only path. */
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define KAST_WIFI_SSID ""
#define KAST_WIFI_PASSWORD ""
#endif

static const char *TAG = "net";

static uint8_t s_retry = 0;
#define KAST_NET_MAX_RETRY     10
#define KAST_NET_SLOW_RETRY_MS 30000

/* Static buffers — the creds path never allocates (K-6). Sized to the
 * driver struct fields (wifi_config_t sta ssid[32] / password[64]). */
static char s_ssid[32];
static char s_pass[64];

static void kast_net_set_creds(const char *ssid, const char *pass)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
}

static void kast_net_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "sta started, connecting to '%s'", s_ssid);
        kast_ui_link_status(KAST_LINK_WIFI, false, "...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        if (s_retry < KAST_NET_MAX_RETRY + 1) {
            s_retry++;   /* clamp so the uint8 never wraps into a 0 ms loop */
        }
        uint32_t backoff_ms;
        if (s_retry <= KAST_NET_MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected (retry %d/%d) reason=%d rssi=%d",
                     s_retry, KAST_NET_MAX_RETRY, disc->reason, disc->rssi);
            kast_ui_link_status(KAST_LINK_WIFI, false, "RETRY");
            backoff_ms = 1000u * s_retry;
            if (backoff_ms > 15000u) {
                backoff_ms = 15000u;
            }
        } else {
            /* Retry budget spent: settle into a slow retry forever.
             * Never esp_restart() here — a wrong/unreachable network is a
             * config problem, and rebooting turned the terminal into a
             * boot loop that reset the box under the user's hands while
             * they were fixing the settings. */
            if (s_retry == KAST_NET_MAX_RETRY + 1) {
                ESP_LOGW(TAG, "retry budget spent — slow retry every %ds (no reboot)",
                         KAST_NET_SLOW_RETRY_MS / 1000);
                kast_ui_link_status(KAST_LINK_WIFI, false, "FAIL");
            }
            backoff_ms = KAST_NET_SLOW_RETRY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_retry = 0;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        kast_ui_link_status(KAST_LINK_WIFI, true, NULL);
    }
}

/* Load creds: NVS first (settings screen), bench header as fallback. */
static esp_err_t kast_net_load_creds(void)
{
    if (kast_config_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi creds from NVS (ssid '%s')", s_ssid);
        return ESP_OK;
    }
    if (strlen(KAST_WIFI_SSID) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    kast_net_set_creds(KAST_WIFI_SSID, KAST_WIFI_PASSWORD);
    ESP_LOGI(TAG, "Wi-Fi creds from bench header (ssid '%s')", s_ssid);
    return ESP_OK;
}

esp_err_t kast_net_init(void)
{
    ESP_RETURN_ON_ERROR(kast_net_load_creds(), TAG,
                        "no Wi-Fi creds — enter them on the settings screen");

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
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", s_ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", s_pass);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "Wi-Fi station up (ssid '%s')", s_ssid);
    return ESP_OK;
}

/* Settings screen save path: new creds go live without a reboot.
 * Runs in LVGL task context; the Wi-Fi API is task-safe. */
void kast_net_apply_creds(const char *ssid, const char *pass)
{
    kast_net_set_creds(ssid, pass);
    s_retry = 0;
    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", s_ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", s_pass);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "applied new creds (ssid '%s'), reconnecting", s_ssid);
}
