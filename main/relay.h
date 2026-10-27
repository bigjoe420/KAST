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

#ifdef __cplusplus
}
#endif
