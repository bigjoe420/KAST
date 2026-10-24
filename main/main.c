/*
 * hello_main.c — Phase 1 boot proof + display bring-up entry.
 * Prints build stamp + chip/PSRAM stats, then brings up the ST7701S panel
 * with a color-bar proof. The build stamp makes the Commandment VI
 * stale-build check trivial: the boot log must show today's date.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_ops.h"

#include "display.h"
#include "touch.h"

/* Bring-up experiment switch: when 1, the RGB display is never initialized.
 * Used to isolate GT911 interference from the running RGB interface —
 * touch works purely over I2C and does not need the panel. */
#ifndef KAST_TOUCH_ONLY
#define KAST_TOUCH_ONLY 0
#endif

static const char *TAG = "kast";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "=== KAST boot proof ===");
    ESP_LOGI(TAG, "BUILD: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "chip: ESP32-S3 rev %d, %d cores @ %d MHz",
             chip.revision, chip.cores, CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "features: WiFi %s, BT %s, embedded-psram %s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_BT) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no");
    ESP_LOGI(TAG, "PSRAM: %d bytes", (int)esp_psram_get_size());

#if !KAST_TOUCH_ONLY
    ESP_ERROR_CHECK(kast_backlight_init());
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(kast_display_init(&panel));
    kast_display_test_pattern(panel);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    kast_backlight_set(100);
    ESP_LOGI(TAG, "display up: color bars expected on screen");
#else
    ESP_LOGI(TAG, "TOUCH-ONLY build: display deliberately off (interference test)");
#endif

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(kast_touch_init(&touch));
    ESP_ERROR_CHECK(kast_touch_debug_start(touch));
    ESP_LOGI(TAG, "touch debug active — tap the screen");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const uint16_t *tx, *ty;
        uint8_t n = kast_touch_last_taps(&tx, &ty);
        uint32_t polls, hits;
        int last_err;
        uint8_t status;
        uint16_t xres, yres;
        kast_touch_stats(&polls, &hits, &last_err);
        kast_touch_raw_diag(&status, &xres, &yres);
        ESP_LOGI(TAG, "alive — polls=%" PRIu32 " hits=%" PRIu32
                 " err=%d raw814E=0x%02X chipres=%dx%d taps=%d",
                 polls, hits, last_err, status, xres, yres, n);
        for (uint8_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  tap[%d]: x=%d y=%d", i, tx[i], ty[i]);
        }
    }
}
