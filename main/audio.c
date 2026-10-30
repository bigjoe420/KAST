/*
 * audio.c — KAST on-board speaker path (NS4168 Class-D on I2S).
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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"

#include "audio.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

/* minimp3 keeps its ~17 KB decoder scratch INSIDE mp3dec_decode_frame's
 * stack frame (mp3dec_scratch_t) — the play task needs generous stack or
 * the decode overflows it and the box reboots (owner-seen 2026-10-27:
 * every SFX tap reset the board). */
#define KAST_AUDIO_STACK   (32 * 1024)
#define KAST_AUDIO_PCM_MAX (2 * 1024 * 1024)   /* PCM working cap, PSRAM */

static const char *TAG = "audio";

/* The speaker is mono but the I2S stream is always stereo: mono MP3s are
 * duplicated sample-by-sample, and a stereo MP3 rides as-is — the amp
 * board picks its channel exactly like the vendor music demo did. */
#define KAST_AUDIO_RATE      44100

static i2s_chan_handle_t s_tx;
static TaskHandle_t      s_play_task;
static StaticTask_t      s_audio_tcb;
static StackType_t      *s_audio_stack;
static QueueHandle_t     s_play_q;   /* one pending mp3 buf+len */
static volatile bool     s_busy;

typedef struct {
    uint8_t *mp3;
    size_t   len;
} kast_audio_job_t;

/* Decode the whole MP3 into a PSRAM PCM buffer, then stream it to the
 * amp. Ownership of job.mp3 always transfers here — freed on every exit
 * path after the queue has taken it. */
static void audio_play_task(void *arg)
{
    (void)arg;
    kast_audio_job_t job;
    for (;;) {
        if (xQueueReceive(s_play_q, &job, portMAX_DELAY) != pdTRUE || !job.mp3) {
            continue;
        }
        s_busy = true;

        int16_t *pcm = heap_caps_malloc(KAST_AUDIO_PCM_MAX, MALLOC_CAP_SPIRAM);
        if (pcm == NULL) {
            pcm = heap_caps_malloc(KAST_AUDIO_PCM_MAX, MALLOC_CAP_8BIT);
        }
        if (pcm == NULL) {
            ESP_LOGE(TAG, "PCM buffer alloc failed (%u bytes)", KAST_AUDIO_PCM_MAX);
            free(job.mp3);
            s_busy = false;
            continue;
        }

        mp3dec_t dec;
        mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        int16_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        size_t samples = 0;   /* per-channel frames accumulated */
        int hz = 0, channels = 0;
        size_t pos = 0;
        while (pos < job.len) {
            int n = mp3dec_decode_frame(&dec, job.mp3 + pos, (int)(job.len - pos),
                                        frame_pcm, &info);
            if (n <= 0) {
                pos += 1;   /* skip a stray byte and resync */
                continue;
            }
            pos += (size_t)info.frame_bytes;
            if (info.hz <= 0 || info.channels < 1) {
                continue;
            }
            if (hz == 0) {
                hz = info.hz;
                channels = info.channels;
            }
            /* minimp3 returns samples PER CHANNEL, regardless of mono/stereo.
             * Dividing by channels discarded half of every stereo MP3 frame. */
            size_t frames = (size_t)n;
            if ((samples + frames) * 2 > KAST_AUDIO_PCM_MAX / sizeof(int16_t)) {
                ESP_LOGW(TAG, "PCM cap hit at %u samples — truncating tail",
                         (unsigned)samples);
                break;
            }
            memcpy(pcm + samples * 2, frame_pcm, frames * 2 * sizeof(int16_t));
            samples += frames;
        }
        free(job.mp3);

        if (samples == 0 || hz == 0) {
            ESP_LOGE(TAG, "no decodable audio in buffer");
            free(pcm);
            s_busy = false;
            continue;
        }
        ESP_LOGI(TAG, "decoded %u frames @ %d Hz x %d ch",
                 (unsigned)samples, hz, channels);

        /* match the stream to the source rate — but only when it
         * actually differs. Reconfiguring an ENABLED std channel is an
         * IDF risk point (clock re-plumb while DMA runs), and every stock
         * sound here is already 44.1 kHz, so the common path must be a
         * no-op. When a reconfig is genuinely needed, do it disabled. */
        if (hz != KAST_AUDIO_RATE) {
            i2s_channel_disable(s_tx);
            i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
            esp_err_t rc = i2s_channel_reconfig_std_clock(s_tx, &clk);
            i2s_channel_enable(s_tx);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "rate reconfig failed — playing at %d Hz anyway", hz);
            }
        }

        /* stereo out: duplicate mono sample-by-sample, ride stereo as-is */
        size_t written_total = 0;
        uint32_t write_calls = 0;
        esp_err_t write_err = ESP_OK;
        if (channels == 1) {
            static int16_t dup[2048];   /* 1024 stereo frames per chunk */
            size_t frames_done = 0;
            while (frames_done < samples) {
                size_t frames = samples - frames_done;
                if (frames > 1024) frames = 1024;
                for (size_t f = 0; f < frames; f++) {
                    dup[f * 2]     = pcm[frames_done + f];
                    dup[f * 2 + 1] = pcm[frames_done + f];
                }
                size_t w = 0;
                esp_err_t rc = i2s_channel_write(s_tx, dup, frames * 2 * sizeof(int16_t),
                                                 &w, portMAX_DELAY);
                write_calls++;
                if (rc != ESP_OK && write_err == ESP_OK) write_err = rc;
                written_total += w;
                frames_done += frames;
            }
        } else {
            size_t bytes = samples * 2 * sizeof(int16_t);
            size_t off = 0;
            while (off < bytes) {
                size_t w = 0;
                esp_err_t rc = i2s_channel_write(s_tx, (const uint8_t *)pcm + off,
                                                 bytes - off, &w, portMAX_DELAY);
                write_calls++;
                if (rc != ESP_OK && write_err == ESP_OK) write_err = rc;
                off += w;
                written_total += w;
            }
        }
        free(pcm);
        s_busy = false;
        ESP_LOGI(TAG, "playback done: %u/%u I2S bytes, %u writes, result %s",
                 (unsigned)written_total,
                 (unsigned)(samples * 2 * sizeof(int16_t)),
                 (unsigned)write_calls, esp_err_to_name(write_err));
    }
}

esp_err_t kast_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = 480;
    chan_cfg.dma_desc_num = 8;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL),
                        TAG, "i2s channel create failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KAST_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 1,      /* docs/PINOUT.md — no exceptions */
            .ws   = 2,
            .dout = 40,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg),
                        TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable failed");

    s_play_q = xQueueCreate(1, sizeof(kast_audio_job_t));
    if (s_play_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* The decoder needs a 32 KB stack, but internal RAM must also retain
     * a contiguous block for the WebSocket task. Keep this known-working
     * layout while display playback is investigated separately. */
    s_audio_stack = heap_caps_malloc(KAST_AUDIO_STACK, MALLOC_CAP_SPIRAM);
    if (s_audio_stack == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_play_task = xTaskCreateStaticPinnedToCore(
        audio_play_task, "kast-audio", KAST_AUDIO_STACK / sizeof(StackType_t),
        NULL, 18, s_audio_stack, &s_audio_tcb, 1);
    if (s_play_task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "speaker path up (I2S0: BCLK=1 WS=2 DOUT=40, %d Hz stereo)",
             KAST_AUDIO_RATE);
    return ESP_OK;
}

esp_err_t kast_audio_play_mp3(uint8_t *mp3, size_t len)
{
    if (s_play_q == NULL || mp3 == NULL || len == 0) {
        if (mp3) free(mp3);
        return ESP_ERR_INVALID_STATE;
    }
    kast_audio_job_t job = { .mp3 = mp3, .len = len };
    /* overwrite any pending job; the running track keeps sounding */
    xQueueOverwrite(s_play_q, &job);
    return ESP_OK;
}

bool kast_audio_busy(void)
{
    return s_busy;
}
