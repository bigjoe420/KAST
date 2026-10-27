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
 * Protocol state: Phase 3 slice 1. Chat frames ({"type":"chat","user","text"})
 * are parsed with a small flat-JSON scanner and pushed into a fixed ring
 * (K-6: nothing allocates per message); screens consume lines with
 * kast_relay_chat_read(). Everything else received is logged and counted.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include <errno.h>
#include "esp_websocket_client.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "relay.h"
#include "ui.h"
#include "config.h"
#include "emote.h"
#include "mbedtls/base64.h"

static const char *TAG = "relay";

#define KAST_RELAY_URI_LEN   96
#define KAST_RELAY_LOG_CHUNK 96   /* log at most this many bytes of a frame */
/* chat frames above KAST_CHAT_FRAME_MAX (emote pushes) reassemble here */
#define KAST_RELAY_BIG_MAX   4096

static esp_websocket_client_handle_t s_client;
static volatile bool s_link_up;   /* CONNECTED/DISCONNECTED events only */
static bool s_ip_held;
static char s_host[64];
static uint16_t s_port;
static uint32_t s_rx_frames;
static uint32_t s_rx_bytes;

/* big-frame reassembly (emote pushes) */
static char     s_big[KAST_RELAY_BIG_MAX + 1];
static size_t   s_big_expect;
static size_t   s_big_off;

static void relay_ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static esp_err_t relay_client_start(void);

/* ------------------------------------------------------------------ */
/* Chat line ring (Phase 3)                                            */
/* ------------------------------------------------------------------ */
/* Written from the websocket event handler, read from the LVGL task:
 * one mutex around short fixed-size copies. The ring never allocates
 * (K-6) and silently drops the oldest line when full. */

#define KAST_CHAT_RING_CAP  32
#define KAST_CHAT_FRAME_MAX 512   /* larger frames are counted+logged, not parsed */

static kast_chat_line_t s_chat_ring[KAST_CHAT_RING_CAP];
static uint32_t s_chat_head;    /* next write slot */
static uint32_t s_chat_count;   /* valid entries */
static uint32_t s_chat_seq;     /* last issued line id */
static uint32_t s_chat_unread;  /* lines pushed since the last chat_read */
static StaticSemaphore_t s_chat_mtx_buf;
static SemaphoreHandle_t s_chat_mtx;

/* moderation ack one-shot ({"type":"mod.ack","action"}) */
static char     s_mod_ack[16];
static volatile bool s_mod_ack_pending;

static void chat_ring_push(const char *id, const char *user, const char *text,
                           const char *emotes)
{
    kast_chat_line_t *slot = &s_chat_ring[s_chat_head];
    snprintf(slot->user, sizeof(slot->user), "%s", user);
    snprintf(slot->text, sizeof(slot->text), "%s", text);
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    snprintf(slot->emotes, sizeof(slot->emotes), "%s", emotes);
    slot->seq = ++s_chat_seq;
    s_chat_head = (s_chat_head + 1) % KAST_CHAT_RING_CAP;
    if (s_chat_count < KAST_CHAT_RING_CAP) {
        s_chat_count++;
    }
    if (s_chat_unread < 0xFFFFFFFFu) {
        s_chat_unread++;
    }
}

uint32_t kast_relay_chat_read(kast_chat_line_t *out, uint32_t max_lines)
{
    if (s_chat_mtx == NULL || out == NULL || max_lines == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_chat_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    uint32_t avail = s_chat_unread;
    if (avail > s_chat_count) {
        avail = s_chat_count;
    }
    if (avail > max_lines) {
        avail = max_lines;
    }
    /* newest `avail` lines, copied oldest-of-batch first */
    uint32_t idx = (s_chat_head + KAST_CHAT_RING_CAP - avail) % KAST_CHAT_RING_CAP;
    for (uint32_t i = 0; i < avail; i++) {
        out[i] = s_chat_ring[idx];
        idx = (idx + 1) % KAST_CHAT_RING_CAP;
    }
    s_chat_unread = 0;
    xSemaphoreGive(s_chat_mtx);
    return avail;
}

/* ------------------------------------------------------------------ */
/* Flat-JSON string scanner                                            */
/* ------------------------------------------------------------------ */
/* Deliberately small: no allocation, no nesting. The relay is the
 * semantic gateway and normalizes frames before they reach the device.
 * Values are skipped after reading so message text can't fake a key,
 * assuming real keys precede the free-text field (the relay's order). */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/* Decode the string content starting at p (just past the opening quote).
 * Escapes resolved: \" \\ \/ kept, \n \t \r become spaces, \uXXXX -> '?'.
 * Control characters become spaces so chat text stays one visual line.
 * Pass out == NULL to just validate and advance. Returns a pointer past
 * the closing quote, or NULL on malformed input. */
static const char *json_parse_string(const char *p, char *out, size_t out_sz)
{
    size_t n = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p != '\0') {
            const char esc = *p++;
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'n':
            case 't':
            case 'r':  c = ' ';  break;
            case 'u':
                for (int i = 0; i < 4 && *p != '\0'; i++) {
                    p++;
                }
                c = '?';
                break;
            default:   c = ' ';  break;
            }
        }
        if ((unsigned char)c < 0x20u) {
            c = ' ';
        }
        if (out != NULL && n + 1 < out_sz) {
            out[n++] = c;
        }
    }
    if (*p != '"') {
        return NULL;
    }
    if (out != NULL) {
        out[n] = '\0';
    }
    return p + 1;
}

/* Find "key": "value" in a flat JSON object. */
static bool json_get_string(const char *json, const char *key,
                            char *out, size_t out_sz)
{
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        char name[24];
        p++;
        const char *after = json_parse_string(p, name, sizeof(name));
        if (after == NULL) {
            return false;
        }
        p = json_skip_ws(after);
        if (*p != ':') {
            continue;   /* a quoted value, not a key */
        }
        p = json_skip_ws(p + 1);
        if (strcmp(name, key) == 0) {
            if (*p != '"') {
                return false;
            }
            return json_parse_string(p + 1, out, out_sz) != NULL;
        }
        if (*p == '"') {
            /* skip this value's content entirely */
            p = json_parse_string(p + 1, NULL, 0);
            if (p == NULL) {
                return false;
            }
        }
    }
    return false;
}

static void strip_char(char *s, char bad)
{
    char *w = s;
    for (const char *r = s; *r != '\0'; r++) {
        if (*r != bad) {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* ------------------------------------------------------------------ */
/* Send path (Phase 3 slice 2)                                         */
/* ------------------------------------------------------------------ */

/* Escape user text for inclusion in a JSON string. Output truncated to
 * out_sz; worst case is 2 bytes per source char. */
static size_t json_escape(const char *src, char *out, size_t out_sz)
{
    size_t n = 0;
    for (const char *p = src; *p != '\0' && n + 2 < out_sz; p++) {
        const unsigned char c = (unsigned char)*p;
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default: break;
        }
        if (esc != NULL) {
            out[n++] = esc[0];
            out[n++] = esc[1];
        } else if (c >= 0x20u) {
            out[n++] = (char)c;
        }
        /* other control chars dropped */
    }
    out[n] = '\0';
    return n;
}

esp_err_t kast_relay_chat_send(const char *text)
{
    if (s_client == NULL || !s_link_up || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char esc[KAST_CHAT_FRAME_MAX / 2];
    char frame[KAST_CHAT_FRAME_MAX];
    json_escape(text, esc, sizeof(esc));
    const int n = snprintf(frame, sizeof(frame),
                           "{\"type\":\"chat.send\",\"text\":\"%s\"}", esc);
    if (n < 0 || n >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int sent = esp_websocket_client_send_text(s_client, frame,
                                                    (int)strlen(frame),
                                                    pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kast_relay_chat_mod(const char *action, const char *id,
                              const char *user)
{
    if (s_client == NULL || !s_link_up || action == NULL || action[0] == '\0' ||
        id == NULL || id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char esc_action[24], esc_id[KAST_CHAT_ID_LEN * 2], esc_user[KAST_CHAT_USER_LEN * 2];
    char frame[KAST_CHAT_FRAME_MAX];
    json_escape(action, esc_action, sizeof(esc_action));
    json_escape(id, esc_id, sizeof(esc_id));
    json_escape(user != NULL ? user : "", esc_user, sizeof(esc_user));
    const int n = snprintf(frame, sizeof(frame),
                           "{\"type\":\"chat.mod\",\"action\":\"%s\","
                           "\"id\":\"%s\",\"user\":\"%s\"}",
                           esc_action, esc_id, esc_user);
    if (n < 0 || n >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int sent = esp_websocket_client_send_text(s_client, frame,
                                                    (int)strlen(frame),
                                                    pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kast_relay_emote_request(const char *code)
{
    if (s_client == NULL || !s_link_up || code == NULL || code[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char esc[KAST_EMOTE_CODE_LEN * 2];
    char frame[96];
    json_escape(code, esc, sizeof(esc));
    const int n = snprintf(frame, sizeof(frame),
                           "{\"type\":\"emote.req\",\"code\":\"%s\"}", esc);
    if (n < 0 || n >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int sent = esp_websocket_client_send_text(s_client, frame,
                                                    (int)strlen(frame),
                                                    pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

bool kast_relay_chat_mod_ack(char *action, size_t len)
{
    if (!s_mod_ack_pending) {
        return false;
    }
    s_mod_ack_pending = false;
    if (action != NULL && len > 0) {
        snprintf(action, len, "%s", s_mod_ack);
    }
    return true;
}

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

/* ------------------------------------------------------------------ */
/* Relay discovery (D-15 Companion: beacon beats typed IPs)          */
/* ------------------------------------------------------------------ */
/* The PC relay (Companion tray app) broadcasts "KAST_RELAY_V1 <port>"
 * to 255.255.255.255:4210 every 2 s. UDP broadcast, not mDNS: port 5353
 * is commonly held on a streaming PC, and a beacon needs no inbound
 * firewall hole — the PC only sends, we only listen, and the packet
 * SOURCE address is the relay's IP on our LAN. Discovery wins over the
 * NVS-saved endpoint so moving locations needs zero settings edits; the
 * saved endpoint is only the fallback when no Companion answers.
 * Blocking recvfrom needs its own task — never call from the event
 * handler. */

#define KAST_RELAY_DISC_TASK_STACK 4096
#define KAST_BEACON_PORT           4210
#define KAST_BEACON_LISTEN_MS      4500   /* ~2 beacon periods + slack */

static void relay_discover_task(void *arg)
{
    (void)arg;
    bool found = false;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(KAST_BEACON_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        struct timeval tv = {
            .tv_sec = KAST_BEACON_LISTEN_MS / 1000,
            .tv_usec = (KAST_BEACON_LISTEN_MS % 1000) * 1000,
        };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0) {
            char buf[64];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            for (;;) {
                const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                           (struct sockaddr *)&from,
                                           &from_len);
                if (n <= 0) {
                    break;   /* timeout or error — no beacon heard */
                }
                buf[n] = '\0';
                unsigned port = 0;
                if (sscanf(buf, "KAST_RELAY_V1 %u", &port) == 1 &&
                    port > 0 && port <= 65535) {
                    const esp_ip4_addr_t ip4 = { .addr = from.sin_addr.s_addr };
                    snprintf(s_host, sizeof(s_host), IPSTR, IP2STR(&ip4));
                    s_port = (uint16_t)port;
                    ESP_LOGI(TAG, "beacon: found KAST relay at %s:%u (auto)",
                             s_host, (unsigned)s_port);
                    found = true;
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "beacon listen bind failed (%d)", errno);
        }
        close(fd);
    }

    if (!found) {
        /* fall back to the settings-screen endpoint (typed or nothing) */
        relay_load_endpoint();
        if (s_host[0] == '\0') {
            ESP_LOGW(TAG, "no relay beacon heard and no endpoint saved — "
                          "run the KAST Companion app on the PC");
            kast_ui_link_status(KAST_LINK_RELAY, false, "SET");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "beacon: none heard — using saved endpoint %s:%u",
                 s_host, (unsigned)s_port);
    }

    if (s_ip_held) {
        relay_client_start();
    }
    vTaskDelete(NULL);
}

static void relay_discover(void)
{
    /* discovery hops tasks on every (re)connect — a tiny detached task
     * per attempt is simpler than recycling one and self-deletes */
    xTaskCreate(relay_discover_task, "kast_disc", KAST_RELAY_DISC_TASK_STACK,
                NULL, 5, NULL);
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
        s_link_up = true;
        ESP_LOGI(TAG, "connected to ws://%s:%u", s_host, (unsigned)s_port);
        kast_ui_link_status(KAST_LINK_RELAY, true, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        /* without this flag a stale handle makes send_text report success
         * on a dead transport — the composer then swallows the message
         * with no error on glass (seen on bench 2026-10-27) */
        s_link_up = false;
        ESP_LOGW(TAG, "disconnected — auto-reconnecting");
        kast_ui_link_status(KAST_LINK_RELAY, false, "...");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x08) {   /* close frame */
            break;
        }
        s_rx_frames++;
        s_rx_bytes += evt->data_len;
        /* chat frames: only parse when the whole payload arrived in one
         * event (relay frames are small; fragmented ones are still
         * counted and logged above the fold) */
        if (evt->fin && evt->payload_len == evt->data_len &&
            evt->payload_len > 0 && evt->payload_len < KAST_CHAT_FRAME_MAX) {
            char frame[KAST_CHAT_FRAME_MAX];
            memcpy(frame, evt->data_ptr, evt->payload_len);
            frame[evt->payload_len] = '\0';
            char type[16];
            if (json_get_string(frame, "type", type, sizeof(type)) &&
                strcmp(type, "chat") == 0) {
                char user[KAST_CHAT_USER_LEN] = "?";
                char text[KAST_CHAT_TEXT_LEN] = "";
                char id[KAST_CHAT_ID_LEN] = "";
                char emotes[40] = "";
                json_get_string(frame, "user", user, sizeof(user));
                json_get_string(frame, "text", text, sizeof(text));
                json_get_string(frame, "id", id, sizeof(id));
                json_get_string(frame, "emotes", emotes, sizeof(emotes));
                /* '#' opens a recolor segment in the chat labels —
                 * strip it until the renderer handles it properly */
                strip_char(user, '#');
                strip_char(text, '#');
                strip_char(id, '#');
                strip_char(emotes, '#');
                xSemaphoreTake(s_chat_mtx, portMAX_DELAY);
                chat_ring_push(id, user, text, emotes);
                xSemaphoreGive(s_chat_mtx);
            } else if (json_get_string(frame, "type", type, sizeof(type)) &&
                       strcmp(type, "mod.ack") == 0) {
                /* bench loop close: relay confirmed a moderation action */
                char action[16] = "";
                json_get_string(frame, "action", action, sizeof(action));
                snprintf(s_mod_ack, sizeof(s_mod_ack), "%s", action);
                s_mod_ack_pending = true;
            }
        } else if (evt->payload_len > KAST_CHAT_FRAME_MAX &&
                   evt->payload_len <= KAST_RELAY_BIG_MAX) {
            /* big frames (emote pushes ~2.2 KB) reassemble across events
             * via payload_offset; the buffer is module-static because the
             * ws event handler is a single task */
            if (evt->payload_offset == 0) {
                s_big_off = 0;
                s_big_expect = evt->payload_len;
            }
            if (s_big_off + (size_t)evt->data_len <= KAST_RELAY_BIG_MAX) {
                memcpy(s_big + s_big_off, evt->data_ptr, evt->data_len);
                s_big_off += (size_t)evt->data_len;
            } else {
                s_big_off = s_big_expect = 0;   /* overflow: drop */
            }
            if (s_big_off != 0 && s_big_off == s_big_expect) {
                s_big[s_big_off] = '\0';
                s_big_off = s_big_expect = 0;
                char type[16];
                if (json_get_string(s_big, "type", type, sizeof(type)) &&
                    strcmp(type, "emote") == 0) {
                    static char code[KAST_EMOTE_CODE_LEN];
                    static char b64[KAST_EMOTE_B64_MAX];
                    static uint8_t raw[KAST_EMOTE_RAW_MAX];
                    size_t raw_len = 0;
                    code[0] = '\0';
                    b64[0] = '\0';
                    json_get_string(s_big, "code", code, sizeof(code));
                    if (code[0] != '\0' &&
                        json_get_string(s_big, "data", b64, sizeof(b64)) &&
                        mbedtls_base64_decode(raw, sizeof(raw), &raw_len,
                                              (const uint8_t *)b64,
                                              strlen(b64)) == 0 &&
                        raw_len == KAST_EMOTE_RAW_MAX) {
                        kast_emote_store(code, raw, raw_len);
                    } else {
                        ESP_LOGW(TAG, "malformed emote frame dropped");
                    }
                }
            }
        }
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
        relay_discover();   /* mDNS first, saved endpoint as fallback */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_held = false;
        relay_client_stop();
    }
}

esp_err_t kast_relay_init(void)
{
    s_chat_mtx = xSemaphoreCreateMutexStatic(&s_chat_mtx_buf);

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

/* ------------------------------------------------------------------ */
/* Settings save path — deferred to a worker task                      */
/* ------------------------------------------------------------------ */
/* Stopping the ws client can block on a dead socket for seconds; run
 * from the LVGL task (settings SAVE) that froze the UI mid-edit. The
 * endpoint reload stays synchronous (quick NVS read); the stop/start
 * hops to a small worker that the apply call just wakes. */

static TaskHandle_t s_apply_task;
static StaticSemaphore_t s_apply_sig_buf;
static SemaphoreHandle_t s_apply_sig;

static void relay_apply_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_apply_sig, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        relay_client_stop();
        if (s_ip_held) {
            relay_discover();   /* re-run: mDNS first, saved endpoint fallback */
        } else if (s_host[0] == '\0') {
            kast_ui_link_status(KAST_LINK_RELAY, false, "SET");
        }
    }
}

esp_err_t kast_relay_apply(void)
{
    ESP_RETURN_ON_ERROR(relay_load_endpoint(), TAG, "endpoint load failed");

    if (s_apply_sig == NULL) {
        s_apply_sig = xSemaphoreCreateBinaryStatic(&s_apply_sig_buf);
        ESP_RETURN_ON_FALSE(s_apply_sig != NULL, ESP_FAIL, TAG, "apply signal create failed");
        ESP_RETURN_ON_FALSE(xTaskCreate(relay_apply_worker, "relay_apply", 4096, NULL,
                                        5, &s_apply_task) == pdPASS,
                            ESP_FAIL, TAG, "apply worker create failed");
    }
    /* Wake the worker (counting-free binary: coalesced rapid SAVEs). A
     * pending-but-unconsumed signal just means the worker runs once more
     * with the freshest endpoint — stop/start is idempotent. */
    xSemaphoreGive(s_apply_sig);
    return ESP_OK;
}
