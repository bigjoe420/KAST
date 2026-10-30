/*
 * relay.h — KAST PC-relay WebSocket client (K-5).
 *
 * Owns the device-side link to the PC relay server (Phase 3 wires the
 * actual chat/OBS protocol on top). Responsibilities: connect when
 * Wi-Fi holds an IP, auto-reconnect with backoff, websocket ping to
 * kill half-open sockets, RELAY status-bar word.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the relay link manager. On IP_EVENT_STA_GOT_IP the link resolves
 * its endpoint from the Companion app's UDP discovery beacon
 * ("KAST_RELAY_V1 <port>" broadcast, relay IP = packet source) and falls
 * back to the NVS-saved endpoint only when no beacon is heard — moving
 * locations needs no settings edits. Tears down while Wi-Fi is down.
 * Call after kast_net_init().
 */
esp_err_t kast_relay_init(void);

/* Settings save path: reload the endpoint from NVS and restart the link
 * if it is running. Safe to call from the LVGL task.
 */
esp_err_t kast_relay_apply(void);

/* ------------------------------------------------------------------ */
/* Chat line delivery (Phase 3)                                        */
/* ------------------------------------------------------------------ */

#define KAST_CHAT_USER_LEN 24
#define KAST_CHAT_TEXT_LEN 168
#define KAST_CHAT_ID_LEN   24   /* relay-normalized message id */

typedef struct {
    char     user[KAST_CHAT_USER_LEN];
    char     text[KAST_CHAT_TEXT_LEN];
    char     id[KAST_CHAT_ID_LEN];   /* platform message id for moderation */
    char     emotes[40];             /* comma-separated codes, ≤3 ("" = none) */
    uint32_t seq;      /* arrival-order id, monotonically increasing */
} kast_chat_line_t;

/* Copy up to max_lines unread chat lines (oldest of the batch first)
 * into out and return how many were copied. The unread cursor is
 * shared: returned lines are consumed, so the next call only sees
 * newer ones. Safe to call from the LVGL task; the backing ring is
 * fixed-size and never allocates (K-6).
 */
uint32_t kast_relay_chat_read(kast_chat_line_t *out, uint32_t max_lines);

/* Send one chat message to the relay as {"type":"chat.send","text"} —
 * the PC side injects it into Kick (D-4: secrets never on device).
 * Safe to call from the LVGL task; blocks up to 2 s on the socket.
 * ESP_ERR_INVALID_STATE when the link is down or text is empty.
 */
esp_err_t kast_relay_chat_send(const char *text);

/* Moderation (Phase 3): send {"type":"chat.mod","action","id","user"}.
 * action is "delete" | "ban" | "timeout". Timeout duration is the relay's
 * default (600 s) in this protocol version — the device doesn't carry it.
 * id is the relay-normalized message id from the chat frame; user is the
 * message author (required for ban/timeout). Safe from the LVGL task.
 * ESP_ERR_INVALID_STATE when the link is down or id is empty.
 */
esp_err_t kast_relay_chat_mod(const char *action, const char *id, const char *user);

/* One-shot consume of the most recent relay mod acknowledgement
 * ({"type":"mod.ack","action"} — the simulate-mode bench loop). Copies the
 * action word into action/len and returns true once per ack; false when
 * nothing new arrived. Call from the LVGL task (chat poll).
 */
bool kast_relay_chat_mod_ack(char *action, size_t len);

/* Emote image channel (Phase 3): ask the relay for one emote code. The
 * relay answers with a {"type":"emote","code","data"} frame whose data is
 * base64 raw RGB565 (KAST_EMOTE_W×H) — landing goes through
 * kast_emote_store() in emote.c. Safe from the LVGL task.
 */
esp_err_t kast_relay_emote_request(const char *code);

/* ------------------------------------------------------------------ */
/* Soundboard (Phase 3)                                                */
/* ------------------------------------------------------------------ */

#define KAST_SFX_MAX       12
#define KAST_SFX_ID_LEN    21   /* wire ids are <= 20 chars */
#define KAST_SFX_NAME_LEN  13   /* wire names are <= 12 chars */

/* How many sounds the relay's catalog currently holds (0..KAST_SFX_MAX).
 * The relay pushes {"type":"sfx.catalog","sounds":"id:Name,..."} on
 * connect; safe to call from the LVGL task. */
uint32_t kast_relay_sfx_count(void);

/* Copy sound idx's id/name out (bounded by the KAST_SFX_*_LEN sizes) and
 * return true, or false when idx is out of range. LVGL task safe. */
bool kast_relay_sfx_get(uint32_t idx, char *id, size_t id_len,
                        char *name, size_t name_len);

/* Bumps every time a catalog frame replaces the store — the SFX screen
 * polls this and rebuilds its grid only on a change. LVGL task safe. */
uint32_t kast_relay_sfx_version(void);

/* Soundboard trigger: send {"type":"sfx.play","id"} — the PC relay plays
 * the matching file from relay/sounds/ on its speakers. Safe to call from
 * the LVGL task; blocks up to 2 s on the socket. ESP_ERR_INVALID_STATE
 * when the link is down or id is empty. */
esp_err_t kast_relay_sfx_play(const char *id);

/* Ask the relay to stream the sound's mp3 down to the device speaker
 * (soundboard monitor path): the relay answers with a run of
 * {"type":"sfx.data","seq","total","end","data"} base64 chunks that
 * relay.c reassembles and hands to kast_audio_play_mp3(). Send right
 * alongside kast_relay_sfx_play() — same rules. */
esp_err_t kast_relay_sfx_audio_req(const char *id);

#ifdef __cplusplus
}
#endif
