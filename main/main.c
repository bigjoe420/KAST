/*
 * main.c — KAST entry point.
 * Boot proof (build stamp + chip/PSRAM stats), then subsystem bring-up:
 * display -> touch -> LVGL UI. The build stamp makes the stale-build check
 * trivial: the boot log must show today's date.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#include "display.h"
#include "touch.h"
#include "ui.h"
#include "net.h"

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

    /* Display: raw RGB panel + vendor ST7701 init stream (display.c).
     * No disp_on_off call: this panel has no DISP GPIO (the RGB engine
     * starts at panel_init; display-on is the vendor 0x29 command). */
    ESP_ERROR_CHECK(kast_backlight_init());
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(kast_display_init(&panel));
    kast_backlight_set(100);

    /* Touch: GT911 (LVGL reads it through the esp_lcd_touch handle). */
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(kast_touch_init(&touch));

    /* UI: LVGL bounce-buffer mode on the RGB panel, then Home screen. */
    ESP_ERROR_CHECK(kast_ui_init(panel, touch));

    /* Wi-Fi station: soft-fail when wifi_config.h has no creds yet —
     * the UI stays up and the status bar shows the state. */
    ESP_ERROR_CHECK(nvs_flash_init());
    const esp_err_t net_err = kast_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init skipped: %s", esp_err_to_name(net_err));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive");
    }
}
