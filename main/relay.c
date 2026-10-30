/*
 * relay.c — KAST PC-relay WebSocket client (see relay.h).
 *
 * Transport state (K-5):
 *   - connect only while Wi-Fi holds an IP (GOT_IP starts, STA_DISCONNECTED
 *     tears down — reconnect is Wi-Fi's job, not ours)
 *   - the websocket client auto-reconnects on transport errors with
 *     reconnect_timeout_ms backoff; ws PING every 10 s + 20 s PONG watch
 *     kill half-open sockets, and a supervisor recreates the client if
 *     the link stays down 30 s (auto-reconnect only covers states the
 *     library's own state machine visits — owner-seen 2026-10-27: link
 *     dropped minutes after boot and nothing reconnected, zero logs)
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
#include "audio.h"
#include "esp_heap_caps.h"
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

/* ------------------------------------------------------------------ */
/* Soundboard catalog (Phase 3)                                        */
/* ------------------------------------------------------------------ */
/* Written from the websocket event handler, read from the LVGL task —
 * same mutex discipline as the chat ring. The relay pushes
 * {"type":"sfx.catalog","sounds":"id:Name,..."} on connect; ids and
 * names arrive comma/colon delimited (the relay strips those characters
 * from file names, so a plain scan is safe). */

static char     s_sfx_id[KAST_SFX_MAX][KAST_SFX_ID_LEN];
static char     s_sfx_name[KAST_SFX_MAX][KAST_SFX_NAME_LEN];
static uint32_t s_sfx_count;
static uint32_t s_sfx_version;
static StaticSemaphore_t s_sfx_mtx_buf;
static SemaphoreHandle_t s_sfx_mtx;

/* Parse one "id:Name" pair into slot idx. Returns false when the pair
 * has no id. Never writes outside the fixed slot buffers. */
static bool sfx_pair_store(uint32_t idx, const char *pair, size_t len)
{
    const char *colon = memchr(pair, ':', len);
    size_t id_len = colon ? (size_t)(colon - pair) : len;
    if (id_len == 0) {
        return false;
    }
    if (id_len >= KAST_SFX_ID_LEN) {
        id_len = KAST_SFX_ID_LEN - 1;
    }
    memcpy(s_sfx_id[idx], pair, id_len);
    s_sfx_id[idx][id_len] = '\0';
    size_t name_len = 0;
    if (colon && (size_t)(colon - pair) + 1 < len) {
        name_len = len - ((size_t)(colon - pair) + 1);
        if (name_len >= KAST_SFX_NAME_LEN) {
            name_len = KAST_SFX_NAME_LEN - 1;
        }
        memcpy(s_sfx_name[idx], colon + 1, name_len);
    }
    s_sfx_name[idx][name_len] = '\0';
    return true;
}

/* Replace the whole store from the wire string. Called with the mutex
 * held (or from init before the link runs). */
static void sfx_catalog_store(const char *sounds)
{
    s_sfx_count = 0;
    while (s_sfx_count < KAST_SFX_MAX && sounds != NULL && sounds[0] != '\0') {
        const char *comma = strchr(sounds, ',');
        size_t len = comma ? (size_t)(comma - sounds) : strlen(sounds);
        if (sfx_pair_store(s_sfx_count, sounds, len)) {
            s_sfx_count++;
        }
        sounds = comma ? comma + 1 : NULL;
    }
    s_sfx_version++;
}

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

/* Find "key": <unsigned integer> in a flat JSON object. Missing or
   non-numeric values leave *out untouched and return false. */
static bool json_get_uint(const char *json, const char *key, uint32_t *out)
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
            continue;
        }
        p = json_skip_ws(p + 1);
        if (strcmp(name, key) == 0) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (uint32_t)(*p - '0');
                p++;
            }
            *out = v;
            return true;
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

uint32_t kast_relay_sfx_count(void)
{
    return s_sfx_count;
}

bool kast_relay_sfx_get(uint32_t idx, char *id, size_t id_len,
                        char *name, size_t name_len)
{
    if (s_sfx_mtx == NULL || idx >= s_sfx_count) {
        return false;
    }
    if (xSemaphoreTake(s_sfx_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (idx < s_sfx_count) {
        if (id != NULL && id_len > 0) {
            snprintf(id, id_len, "%s", s_sfx_id[idx]);
        }
        if (name != NULL && name_len > 0) {
            snprintf(name, name_len, "%s", s_sfx_name[idx]);
        }
        xSemaphoreGive(s_sfx_mtx);
        return true;
    }
    xSemaphoreGive(s_sfx_mtx);
    return false;
}

uint32_t kast_relay_sfx_version(void)
{
    return s_sfx_version;
}

esp_err_t kast_relay_sfx_play(const char *id)
{
    if (s_client == NULL || !s_link_up || id == NULL || id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char esc[KAST_SFX_ID_LEN];
    char frame[96];
    json_escape(id, esc, sizeof(esc));
    const int n = snprintf(frame, sizeof(frame),
                           "{\"type\":\"sfx.play\",\"id\":\"%s\"}", esc);
    if (n < 0 || n >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int sent = esp_websocket_client_send_text(s_client, frame,
                                                    (int)strlen(frame),
                                                    pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kast_relay_sfx_audio_req(const char *id)
{
    if (s_client == NULL || !s_link_up || id == NULL || id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char esc[KAST_SFX_ID_LEN];
    char frame[112];
    json_escape(id, esc, sizeof(esc));
    const int n = snprintf(frame, sizeof(frame),
                           "{\"type\":\"sfx.audio.req\",\"id\":\"%s\"}", esc);
    if (n < 0 || n >= (int)sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int sent = esp_websocket_client_send_text(s_client, frame,
                                                    (int)strlen(frame),
                                                    pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* SFX audio delivery: relay streams the mp3 in base64 chunks; the ws  */
/* handler reassembles into a PSRAM buffer and hands it to the audio   */
/* task (ownership transfers — audio.c frees it).                      */
/* ------------------------------------------------------------------ */

#define KAST_SFX_MP3_MAX   (640 * 1024)

static uint8_t *s_sfx_mp3;         /* reassembly buffer, PSRAM */
static size_t   s_sfx_mp3_cap;
static size_t   s_sfx_mp3_len;     /* declared total of the current run */
static size_t   s_sfx_mp3_off;     /* bytes written so far */
static uint32_t s_sfx_mp3_seq;     /* next expected chunk */

static void sfx_mp3_reset(void)
{
    if (s_sfx_mp3 != NULL) {
        free(s_sfx_mp3);
        s_sfx_mp3 = NULL;
    }
    s_sfx_mp3_cap = s_sfx_mp3_len = s_sfx_mp3_off = 0;
    s_sfx_mp3_seq = 0;
}

/* Parse one reassembled sfx.data frame. Runs on the ws event task —
 * keep it copies-and-accounting only; decode + playback live on the
 * audio task. */
static void sfx_mp3_on_frame(const char *frame)
{
    /* static, not stack: this runs ON websocket_task (4 KB->8 KB stack)
     * and a 3.9 KB array here overflowed that task on every sound tap —
     * coredump-verified 2026-10-27. The ws event handler is a single
     * task, same justification as s_big and the emote statics. */
    static char data[3900];
    data[0] = '\0';
    if (!json_get_string(frame, "data", data, sizeof(data))) {
        return;
    }
    uint32_t seq = 0, total = 0, end = 0;
    json_get_uint(frame, "seq", &seq);
    json_get_uint(frame, "total", &total);
    json_get_uint(frame, "end", &end);
    if (total == 0 || total > KAST_SFX_MP3_MAX) {
        return;
    }
    if (seq == 0) {
        sfx_mp3_reset();
        s_sfx_mp3 = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
        if (s_sfx_mp3 == NULL) {
            s_sfx_mp3 = heap_caps_malloc(total, MALLOC_CAP_8BIT);
        }
        if (s_sfx_mp3 == NULL) {
            ESP_LOGW(TAG, "sfx mp3 buffer alloc failed (%u bytes)", (unsigned)total);
            return;
        }
        s_sfx_mp3_cap = total;
        s_sfx_mp3_len = total;
        s_sfx_mp3_seq = 0;
    }
    if (s_sfx_mp3 == NULL || seq != s_sfx_mp3_seq || total != s_sfx_mp3_len) {
        return;   /* stale/mismatched chunk — the run is dead, next tap retries */
    }
    size_t b64_len = strlen(data);
    size_t room = s_sfx_mp3_cap - s_sfx_mp3_off;
    size_t out_len = 0;
    if (mbedtls_base64_decode(s_sfx_mp3 + s_sfx_mp3_off, room, &out_len,
                              (const uint8_t *)data,
                              b64_len) != 0) {
        ESP_LOGW(TAG, "sfx chunk %u: bad base64", (unsigned)seq);
        return;
    }
    s_sfx_mp3_off += out_len;
    s_sfx_mp3_seq++;
    if (end && s_sfx_mp3_off == s_sfx_mp3_len) {
        uint8_t *buf = s_sfx_mp3;
        size_t len = s_sfx_mp3_off;
        s_sfx_mp3 = NULL;
        s_sfx_mp3_cap = s_sfx_mp3_len = s_sfx_mp3_off = 0;
        s_sfx_mp3_seq = 0;
        ESP_LOGI(TAG, "sfx mp3 complete (%u bytes) — handing to audio task",
                 (unsigned)len);
        kast_audio_play_mp3(buf, len);   /* takes ownership; frees on exit */
    }
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
        /* One shot used to be it: if the ws task-create failed (RAM
         * crunch right after a crash-reboot), the box sat with NO RELAY
         * until the next reboot — owner-seen 2026-10-27. Retry with
         * backoff and heap telemetry until it sticks. */
        for (int attempt = 1; ; attempt++) {
            const esp_err_t err = relay_client_start();
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "relay client start failed (%s), attempt %d — "
                          "retrying in 5 s (free %u, largest block %u)",
                     esp_err_to_name(err), attempt,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
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
        /* The 4 KB default overflows the moment a chunky frame arrives:
         * the EVENT_DATA handler chain runs ON websocket_task and the sfx
         * path alone parked a 3.9 KB base64 buffer in a stack frame
         * (coredump-verified 2026-10-27: "stack overflow in task
         * websocket_task" on every sound tap). The buffer is static now.
         * Don't bump this casually: every extra KB makes the task-create
         * allocation harder after a crash-reboot, and a start failure
         * used to mean NO RELAY until reboot (retry loop below covers
         * that now). */
        .task_stack = 8192,
        /* Half-open killer (K-5), explicit on purpose: the library would
         * default to ping 10 s + PONG watch 120 s — a dead NAT entry then
         * keeps the status bar dark for two minutes. 20 s detects it
         * quickly; ws PINGs also keep the hotspot NAT mapping alive. */
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 20,
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

/* ------------------------------------------------------------------ */
/* Link supervisor (K-5 hardening)                                     */
/* ------------------------------------------------------------------ */
/* The library's auto-reconnect only fires from states its own run loop
 * visits. Owner-seen 2026-10-27: minutes after a crash-reboot the link
 * dropped, the relay logged "device disconnected", and the box sat with
 * a dark RELAY word until power-cycle — alive heartbeats from other
 * tasks, zero ws log lines, so the client was wedged in a state the
 * backoff never revisits. This supervisor force-recreates the client
 * when the link has been down too long. Static task (K-6: no hot-path
 * heap churn — the ws task-create is exactly what RAM crunch kills).
 *
 * stop/start are idempotent against our other callers (apply worker,
 * Wi-Fi events) via the s_client NULL gate; a worst-case double restart
 * just costs one extra reconnect. */

#define KAST_LINK_SUPERVISE_MS 5000     /* check cadence */
#define KAST_LINK_DOWN_LIMIT_MS 30000   /* down this long -> recreate */
#define KAST_LINK_SUPER_STACK (8 * 1024)

static StaticTask_t s_super_tcb;
static StackType_t s_super_stack[KAST_LINK_SUPER_STACK / sizeof(StackType_t)];

static void relay_supervisor_task(void *arg)
{
    (void)arg;
    uint32_t down_since_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KAST_LINK_SUPERVISE_MS));
        if (!s_ip_held || s_link_up) {
            down_since_ms = 0;
            continue;
        }
        const uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
        if (down_since_ms == 0) {
            down_since_ms = now_ms;
            continue;
        }
        if (now_ms - down_since_ms < KAST_LINK_DOWN_LIMIT_MS) {
            continue;
        }
        ESP_LOGW(TAG, "link supervisor: relay link down %u s — recreating ws client",
                 (unsigned)((now_ms - down_since_ms) / 1000));
        down_since_ms = now_ms;   /* re-arm; CONNECTED clears this via s_link_up */
        if (s_client != NULL) {
            relay_client_stop();
        }
        relay_client_start();   /* failure leaves s_client NULL — retried next tick */
    }
}

static void relay_supervisor_start(void)
{
    if (xTaskCreateStatic(relay_supervisor_task, "relay_super",
                          KAST_LINK_SUPER_STACK / sizeof(StackType_t),
                          NULL, 5, s_super_stack, &s_super_tcb) == NULL) {
        ESP_LOGW(TAG, "link supervisor create failed — auto-reconnect only");
    }
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
        /* The relay log retains the boot/reset history across reconnects. */
        {
            char hello[128];
            const int hn = snprintf(hello, sizeof(hello),
                                    "{\"type\":\"dev.hello\",\"reset\":\"%s\"}",
                                    kast_config_last_reset_str());
            if (hn > 0 && hn < (int)sizeof(hello)) {
                esp_websocket_client_send_text(s_client, hello, hn,
                                               pdMS_TO_TICKS(2000));
            }
        }
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
            } else if (json_get_string(frame, "type", type, sizeof(type)) &&
                       strcmp(type, "sfx.catalog") == 0) {
                /* soundboard catalog, pushed on connect: comma-separated
                   "id:Name" pairs into the fixed store */
                char sounds[480] = "";
                json_get_string(frame, "sounds", sounds, sizeof(sounds));
                xSemaphoreTake(s_sfx_mtx, portMAX_DELAY);
                sfx_catalog_store(sounds);
                xSemaphoreGive(s_sfx_mtx);
            } else if (json_get_string(frame, "type", type, sizeof(type)) &&
                       strcmp(type, "obs.status") == 0) {
                char status[20] = "";
                json_get_string(frame, "status", status, sizeof(status));
                const bool ok = strcmp(status, "connected") == 0;
                kast_ui_link_status(KAST_LINK_OBS, ok,
                                    ok ? NULL : (strcmp(status, "auth_failed") == 0 ? "AUTH" : "--"));
            } else if (json_get_string(frame, "type", type, sizeof(type)) &&
                       strcmp(type, "sfx.ack") == 0) {
                /* A failed trigger is visible even when the transport and
                   OBS socket are healthy (missing media, rejected request). */
                if (strstr(frame, "\"ok\":false") != NULL) {
                    kast_ui_link_status(KAST_LINK_OBS, false, "FAIL");
                }
            } else if (json_get_string(frame, "type", type, sizeof(type)) &&
                       strcmp(type, "sfx.data") == 0) {
                /* legacy frames are ignored: soundboard audio lives in OBS */
                ESP_LOGW(TAG, "retired sfx.data frame ignored");
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
                } else if (json_get_string(s_big, "type", type, sizeof(type)) &&
                           strcmp(type, "sfx.data") == 0) {
                    /* soundboard mp3 chunk — reassemble in PSRAM */
                    sfx_mp3_on_frame(s_big);
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
    s_sfx_mtx = xSemaphoreCreateMutexStatic(&s_sfx_mtx_buf);

    ESP_RETURN_ON_ERROR(relay_load_endpoint(), TAG, "endpoint load failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &relay_net_handler, NULL),
                        TAG, "ip event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   &relay_net_handler, NULL),
                        TAG, "wifi event register failed");

    relay_supervisor_start();

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
