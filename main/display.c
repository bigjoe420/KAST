/*
 * display.c — KAST display bring-up.
 *
 * Reference chain (PINOUT.md is the Law):
 *  - RGB data/control pins:      pins.h (vendor IO XLSX + demo, 2026-10-21)
 *  - Timing table:               vendor Arduino_GFX demo (HFP10/HPW8/HBP50,
 *                                VFP10/VPW8/VBP20, 16 MHz PCLK)
 *  - Color order:                BGR (vendor demo init flag)
 *  - ST7701 init sequence:       vendor type1 ops, transcribed 1:1 from
 *                                Arduino_ST7701_RGBPanel.h (the factory demo's
 *                                proven sequence for this exact glass).
 *                                The component default is for Espressif's
 *                                reference display — wrong gamma/power for
 *                                this panel produces a black screen.
 *
 * Gotchas baked in here (see PROGRESS.md for the war stories):
 *  - 3-wire SPI runs in MODE 0 (Espressif ST7701 example): the SCL active
 *    edge selects the sampling edge; mode 3 garbles every init command.
 *  - Framebuffers live in PSRAM behind the CPU cache: after writing pixels
 *    directly, esp_cache_msync(DIR_C2M) is mandatory or the RGB DMA keeps
 *    streaming stale (zeroed) lines — screen stays black.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"

#include "pins.h"
#include "display.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;

/*
 * Vendor type1 ST7701 init — transcribed 1:1 from the factory demo
 * (Arduino_ST7701_RGBPanel.h, st7701_type1_init_operations).
 * Gamma, power (Vop/VCOM/VGH/VGL) and GIP timings are glass-specific;
 * the component's generic default leaves this panel black.
 */
static const st7701_lcd_init_cmd_t s_vendor_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                       0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                       0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x32}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                       0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                       0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                       0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                       0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE5, (uint8_t[]){0xE4}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x60}, 1, 10},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

esp_err_t kast_display_init(esp_lcd_panel_handle_t *out_panel)
{
    esp_err_t ret = ESP_OK;

    /* --- 3-wire SPI command channel (CS=39, SCL=48, SDA=47) --- */
    const spi_line_config_t lines = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = KAST_LCD_CS,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = KAST_LCD_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = KAST_LCD_SDA,
        .io_expander = NULL,
    };
    const esp_lcd_panel_io_3wire_spi_config_t io_cfg =
        ST7701_PANEL_IO_3WIRE_SPI_CONFIG(lines, 0 /* scl_active_edge -> SPI mode 0, per Espressif ST7701 example */);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io), err, TAG,
                      "3-wire SPI panel IO init failed");

    /* --- RGB panel timing: vendor table (pins.h) --- */
    const esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M, /* vendor Arduino_GFX uses PLL160M —
                                         LCD_CLK_SRC_DEFAULT (PLL240M on S3)
                                         emits a different noise spectrum that
                                         drowns the GT911 on this shared-flex
                                         board (touch dead with display on) */
        .timings = {
            .pclk_hz = KAST_LCD_PCLK_HZ,
            .h_res = 480,
            .v_res = 480,
            .hsync_pulse_width = KAST_LCD_HPW,
            .hsync_back_porch = KAST_LCD_HBP,
            .hsync_front_porch = KAST_LCD_HFP,
            .vsync_pulse_width = KAST_LCD_VPW,
            .vsync_back_porch = KAST_LCD_VBP,
            .vsync_front_porch = KAST_LCD_VFP,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1, /* vendor demo runs a single PSRAM framebuffer */
        .bounce_buffer_size_px = 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = KAST_LCD_HSYNC,
        .vsync_gpio_num = KAST_LCD_VSYNC,
        .de_gpio_num = KAST_LCD_DE,
        .pclk_gpio_num = KAST_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            KAST_LCD_B0, KAST_LCD_B1, KAST_LCD_B2, KAST_LCD_B3, KAST_LCD_B4,
            KAST_LCD_G0, KAST_LCD_G1, KAST_LCD_G2, KAST_LCD_G3, KAST_LCD_G4,
            KAST_LCD_G5,
            KAST_LCD_R0, KAST_LCD_R1, KAST_LCD_R2, KAST_LCD_R3, KAST_LCD_R4,
        },
        .flags.fb_in_psram = true,
    };

    const st7701_vendor_config_t vendor_cfg = {
        .init_cmds = s_vendor_init_cmds,
        .init_cmds_size = sizeof(s_vendor_init_cmds) / sizeof(st7701_lcd_init_cmd_t),
        .rgb_config = &rgb_cfg,
        .flags = {
            .mirror_by_cmd = 0,
            .auto_del_panel_io = 0, /* dedicated cmd lines — no multiplexing */
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_cfg, &s_panel),
                      err, TAG, "ST7701 panel create failed");
    /* Vendor demo resets (SWRESET via 3-wire SPI) before init — without it
     * the ST7701 can be left in an unknown state and ignores the init. */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), err, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), err, TAG, "panel init failed");

    *out_panel = s_panel;
    ESP_LOGI(TAG, "display init OK (480x480 RGB565, 2 PSRAM framebuffers)");
    return ESP_OK;

err:
    return ret;
}

esp_err_t kast_backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "LEDC timer failed");

    ledc_channel_config_t ccfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = KAST_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "LEDC channel failed");
    return ESP_OK;
}

void kast_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void kast_display_test_pattern(esp_lcd_panel_handle_t panel)
{
    void *fb0 = NULL;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb0));
    if (!fb0) {
        ESP_LOGE(TAG, "no framebuffer");
        return;
    }

    /* Mostly-static image on purpose: the old 8-bar pattern flipped all 16
     * RGB data lines every 16 px at full PCLK rate — worst-case switching
     * noise on the shared flex, enough to deafen the GT911 analog front-end
     * (touch dead with display on, alive with display off). A real UI is
     * mostly static pixels, so proof-of-life now draws a calm screen:
     * solid dark blue + a white anchor square in the top-left corner. */
    uint16_t *px = (uint16_t *)fb0;
    for (int i = 0; i < 480 * 480; i++) {
        px[i] = 0x0008; /* near-black blue */
    }
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 60; x++) {
            px[y * 480 + x] = 0xFFFF;
        }
    }
    /* PSRAM framebuffer sits behind the CPU cache — push writes to memory
     * or the RGB DMA streams the stale zeroed buffer (black screen). */
    esp_cache_msync(fb0, 480 * 480 * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ESP_LOGI(TAG, "test pattern drawn");
}
