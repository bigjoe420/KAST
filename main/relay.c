/*
 * relay.c — KAST PC-relay WebSocket client (see relay.h).
 *
 * Transport state (K-5):
 *   - connect only while Wi-Fi holds an IP (GOT_IP starts, STA_DISCONNECTED
 *     tears down — reconnect is Wi-Fi's job, not ours)
 *   - the websocket client auto-reconnects on transport errors with
 *     reconnect_timeout_ms backoff; ping every 10 s kills half-opens
 *   - every state change lands in the status-bar RELAY word
 *
 * Protocol state: Phase 3. Until the relay exists, received frames are
 * logged and counted so the bench can prove the path with any ws echo.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_websocket_client.h"

#include "relay.h"
#include "ui.h"
#include "config.h"

static const char *TAG = "relay";

#define KAST_RELAY_URI_LEN   96
#define KAST_RELAY_LOG_CHUNK 96   /* log at most this many bytes of a frame */

static esp_websocket_client_handle_t s_client;
static bool s_ip_held;
static char s_host[64];
static uint16_t s_port;
static uint32_t s_rx_frames;
static uint32_t s_rx_bytes;

static void relay_ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static esp_err_t relay_load_endpoint(void)
{
    esp_err_t err = kast_config_load_relay(s_host, sizeof(s_host), &s_port);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_host[0] = '\0';
        s_port = 0;
        return ESP_OK;   /* not configured — a valid state, not an error */
    }
    return err;
}

static esp_err_t relay_client_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }
    if (s_host[0] == '\0' || s_port == 0) {
        ESP_LOGW(TAG, "relay endpoint not set — configure host/port on the settings screen");
        kast_ui_link_status(KAST_LINK_RELAY, false, "SET");
        return ESP_ERR_INVALID_STATE;
    }

    char uri[KAST_RELAY_URI_LEN];
    snprintf(uri, sizeof(uri), "ws://%s:%u", s_host, (unsigned)s_port);

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 3000,
        /* ping_interval_sec defaults to 10 s — half-open killer (K-5) */
    };
    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_FAIL, TAG, "client init failed");
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                      relay_ws_handler, NULL),
                        TAG, "event register failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_client), TAG, "client start failed");
    ESP_LOGI(TAG, "connecting to %s", uri);
    kast_ui_link_status(KAST_LINK_RELAY, false, "...");
    return ESP_OK;
}

static void relay_client_stop(void)
{
    if (s_client == NULL) {
        return;
    }
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    kast_ui_link_status(KAST_LINK_RELAY, false, NULL);
}

static void relay_ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *evt =
        (const esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to ws://%s:%u", s_host, (unsigned)s_port);
        kast_ui_link_status(KAST_LINK_RELAY, true, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected — auto-reconnecting");
        kast_ui_link_status(KAST_LINK_RELAY, false, "...");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x08) {   /* close frame */
            break;
        }
        s_rx_frames++;
        s_rx_bytes += evt->payload_len;
        ESP_LOGI(TAG, "frame #%u (%u bytes, fin=%d): %.*s",
                 (unsigned)s_rx_frames, (unsigned)evt->payload_len, evt->fin,
                 evt->data_len < KAST_RELAY_LOG_CHUNK ? (int)evt->data_len
                                                       : KAST_RELAY_LOG_CHUNK,
                 (const char *)evt->data_ptr);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "transport error");
        break;
    default:
        break;
    }
}

/* Wi-Fi state drives the link: up with an IP -> connect, down -> teardown.
 * Runs on the default event loop (system task), not the LVGL task. */
static void relay_net_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ip_held = true;
        relay_client_start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_held = false;
        relay_client_stop();
    }
}

esp_err_t kast_relay_init(void)
{
    ESP_RETURN_ON_ERROR(relay_load_endpoint(), TAG, "endpoint load failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &relay_net_handler, NULL),
                        TAG, "ip event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   &relay_net_handler, NULL),
                        TAG, "wifi event register failed");

    if (s_host[0] == '\0') {
        ESP_LOGW(TAG, "no relay endpoint configured yet — settings screen sets it");
        kast_ui_link_status(KAST_LINK_RELAY, false, "SET");
    } else {
        ESP_LOGI(TAG, "relay endpoint %s:%u (connects when Wi-Fi is up)",
                 s_host, (unsigned)s_port);
    }
    return ESP_OK;
}

esp_err_t kast_relay_apply(void)
{
    ESP_RETURN_ON_ERROR(relay_load_endpoint(), TAG, "endpoint load failed");

    relay_client_stop();
    if (s_host[0] == '\0') {
        kast_ui_link_status(KAST_LINK_RELAY, false, "SET");
        return ESP_OK;
    }
    return s_ip_held ? relay_client_start() : ESP_OK;
}
