/*
 * audio.h — KAST on-board speaker path (NS4168 Class-D on I2S).
 *
 * Datasheet pins (docs/PINOUT.md): BCLK=1, LRCK(WS)=2, DOUT=40, single
 * 4-ohm speaker on the MX1.25 header. Playback decodes an MP3 buffer in
 * PSRAM via minimp3 (public domain, main/minimp3.h) and streams PCM to
 * the amp from a dedicated task — the LVGL task and the ws event task
 * never block on audio.
 *
 * Reserved for local interface sounds and alerts. Soundboard taps stay
 * on the stream PC and do not transfer or decode audio on the device.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the I2S TX channel. Call once during app init, after the
 * display/touch are running (pin law: 1/2/40 are free — see
 * docs/PINOUT.md). */
esp_err_t kast_audio_init(void);

/* Decode an MP3 buffer and play it on the speaker. The buffer must be
 * heap_caps_malloc'd (PSRAM preferred) and is FREED from the play task
 * when playback finishes — callers hand over ownership. Returns
 * ESP_ERR_INVALID_STATE when the audio path is not up. Safe from any
 * task; a new request replaces a pending track while the current one
 * finishes. */
esp_err_t kast_audio_play_mp3(uint8_t *mp3, size_t len);

/* true while a track is still sounding. */
bool kast_audio_busy(void);

#ifdef __cplusplus
}
#endif
