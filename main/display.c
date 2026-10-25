/*
 * display.c — KAST display bring-up (ST7701S over a raw RGB panel).
 *
 * Reference chain (PINOUT.md is the Law):
 *  - RGB data/control pins:      pins.h (vendor IO XLSX + demo, 2026-10-21)
 *  - Timing table:               vendor Arduino_GFX demo (HFP10/HPW8/HBP50,
 *                                VFP10/VPW8/VBP20, 12 MHz PCLK, PLL160M)
 *  - Color order:                BGR (vendor demo init flag)
 *  - ST7701 init sequence:       vendor type1 ops, transcribed 1:1 from
 *                                Arduino_ST7701_RGBPanel.h (the factory demo's
 *                                proven sequence for this exact glass).
 *                                The component default is for Espressif's
 *                                reference display — wrong gamma/power for
 *                                this panel produces a black screen.
 *
 * The RGB panel is created directly with esp_lcd_new_rgb_panel() (not via the
 * esp_lcd_st7701 wrapper): LVGL's RGB port needs the raw panel handle so it
 * can use the internal framebuffers as draw buffers (tear-free mode). The
 * vendor init stream is sent over the 3-wire SPI command channel instead —
 * same bytes the component would send.
 *
 * Gotchas baked in here (learned the hard way on the bench):
 *  - 3-wire SPI runs in MODE 0 (Espressif ST7701 example): the SCL active
 *    edge selects the sampling edge; mode 3 garbles every init command.
 *  - SWRESET (0x01) before the init stream — without it the ST7701 can be
 *    left in an unknown state and ignores init.
 *  - Animated/busy patterns deafen the GT911 on this shared-flex board
 *    (switching-noise EMI). Static UIs only.
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
#include "esp_lcd_panel_io.h"
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
 * `delay_ms` after each command is part of the sequence (0x11 sleep-out
 * needs 120 ms before display-on).
 */
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_bytes;
    uint16_t delay_ms;
} kast_lcd_init_cmd_t;

static const uint8_t vendor_ff1[] = {0x77, 0x01, 0x00, 0x00, 0x10};
static const uint8_t vendor_c0[]  = {0x3B, 0x00};
static const uint8_t vendor_c1[]  = {0x0D, 0x02};
static const uint8_t vendor_c2[]  = {0x31, 0x05};
static const uint8_t vendor_cd[]  = {0x00};
static const uint8_t vendor_b0[]  = {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                                     0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18};
static const uint8_t vendor_b1[]  = {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                                     0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18};
static const uint8_t vendor_ff2[] = {0x77, 0x01, 0x00, 0x00, 0x11};
static const uint8_t vendor_b0b[] = {0x60};
static const uint8_t vendor_b1b[] = {0x32};
static const uint8_t vendor_b2[]  = {0x07};
static const uint8_t vendor_b3[]  = {0x80};
static const uint8_t vendor_b5[]  = {0x49};
static const uint8_t vendor_b7[]  = {0x85};
static const uint8_t vendor_b8[]  = {0x21};
static const uint8_t vendor_c1b[] = {0x78};
static const uint8_t vendor_c2b[] = {0x78};
static const uint8_t vendor_e0[]  = {0x00, 0x1B, 0x02};
static const uint8_t vendor_e1[]  = {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                                     0x00, 0x44, 0x44};
static const uint8_t vendor_e2[]  = {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                                     0xEC, 0xA0, 0x00, 0x00};
static const uint8_t vendor_e3[]  = {0x00, 0x00, 0x11, 0x11};
static const uint8_t vendor_e4[]  = {0x44, 0x44};
static const uint8_t vendor_e5[]  = {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                                     0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0};
static const uint8_t vendor_e6[]  = {0x00, 0x00, 0x11, 0x11};
static const uint8_t vendor_e7[]  = {0x44, 0x44};
static const uint8_t vendor_e8[]  = {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                                     0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0};
static const uint8_t vendor_eb[]  = {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40};
static const uint8_t vendor_ec[]  = {0x3C, 0x00};
static const uint8_t vendor_ed[]  = {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA};
static const uint8_t vendor_ff3[] = {0x77, 0x01, 0x00, 0x00, 0x13};
static const uint8_t vendor_e5b[] = {0xE4};
static const uint8_t vendor_ff4[] = {0x77, 0x01, 0x00, 0x00, 0x00};
static const uint8_t vendor_3a[]  = {0x60};

static const kast_lcd_init_cmd_t s_vendor_init_cmds[] = {
    {0xFF, vendor_ff1, sizeof(vendor_ff1), 0},
    {0xC0, vendor_c0,  sizeof(vendor_c0),  0},
    {0xC1, vendor_c1,  sizeof(vendor_c1),  0},
    {0xC2, vendor_c2,  sizeof(vendor_c2),  0},
    {0xCD, vendor_cd,  sizeof(vendor_cd),  0},
    {0xB0, vendor_b0,  sizeof(vendor_b0),  0},
    {0xB1, vendor_b1,  sizeof(vendor_b1),  0},
    {0xFF, vendor_ff2, sizeof(vendor_ff2), 0},
    {0xB0, vendor_b0b, sizeof(vendor_b0b), 0},
    {0xB1, vendor_b1b, sizeof(vendor_b1b), 0},
    {0xB2, vendor_b2,  sizeof(vendor_b2),  0},
    {0xB3, vendor_b3,  sizeof(vendor_b3),  0},
    {0xB5, vendor_b5,  sizeof(vendor_b5),  0},
    {0xB7, vendor_b7,  sizeof(vendor_b7),  0},
    {0xB8, vendor_b8,  sizeof(vendor_b8),  0},
    {0xC1, vendor_c1b, sizeof(vendor_c1b), 0},
    {0xC2, vendor_c2b, sizeof(vendor_c2b), 0},
    {0xE0, vendor_e0,  sizeof(vendor_e0),  0},
    {0xE1, vendor_e1,  sizeof(vendor_e1),  0},
    {0xE2, vendor_e2,  sizeof(vendor_e2),  0},
    {0xE3, vendor_e3,  sizeof(vendor_e3),  0},
    {0xE4, vendor_e4,  sizeof(vendor_e4),  0},
    {0xE5, vendor_e5,  sizeof(vendor_e5),  0},
    {0xE6, vendor_e6,  sizeof(vendor_e6),  0},
    {0xE7, vendor_e7,  sizeof(vendor_e7),  0},
    {0xE8, vendor_e8,  sizeof(vendor_e8),  0},
    {0xEB, vendor_eb,  sizeof(vendor_eb),  0},
    {0xEC, vendor_ec,  sizeof(vendor_ec),  0},
    {0xED, vendor_ed,  sizeof(vendor_ed),  0},
    {0xFF, vendor_ff3, sizeof(vendor_ff3), 0},
    {0xE5, vendor_e5b, sizeof(vendor_e5b), 0},
    {0xFF, vendor_ff4, sizeof(vendor_ff4), 0},
    {0x3A, vendor_3a,  sizeof(vendor_3a),  10},
    {0x11, NULL,       0,                   120},  /* sleep out */
    {0x29, NULL,       0,                   0},     /* display on */
};

/* Push the vendor init stream over the 3-wire SPI command channel. */
static esp_err_t st7701_send_init(esp_lcd_panel_io_handle_t io)
{
    /* SWRESET first — a panel left in an unknown state ignores init. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x01, NULL, 0), TAG,
                        "SWRESET failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(s_vendor_init_cmds) / sizeof(s_vendor_init_cmds[0]); i++) {
        const kast_lcd_init_cmd_t *c = &s_vendor_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->data_bytes),
                            TAG, "init cmd 0x%02X failed", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

esp_err_t kast_display_init(esp_lcd_panel_handle_t *out_panel)
{
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io), TAG,
                        "3-wire SPI panel IO init failed");

    /* --- Raw RGB panel (vendor timing table, pins.h) ---
     * Two PSRAM framebuffers: LVGL's tear-free mode adopts them as its own
     * draw buffers and swaps on VSYNC. */
    const esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M, /* vendor Arduino_GFX clock — DEFAULT
                                         (PLL240M) emits a noise spectrum that
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
        .num_fbs = 2, /* required by LVGL avoid_tearing (both fbs adopted) */
        /* Bounce buffers: DMA stages frame lines from small INTERNAL-RAM
         * buffers instead of reading PSRAM at pixel-clock rate. Without this
         * the shared PSRAM bus underruns (bad flicker; same root cause as
         * the earlier drifting bars). */
        .bounce_buffer_size_px = 480 * 10,
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel), TAG,
                        "RGB panel create failed");

    /* Vendor ST7701 init BEFORE the RGB engine starts streaming. */
    ESP_RETURN_ON_ERROR(st7701_send_init(io), TAG, "vendor init stream failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "RGB panel init failed");

    *out_panel = s_panel;
    ESP_LOGI(TAG, "display init OK (480x480 RGB565, 2 PSRAM framebuffers, 12MHz PLL160M)");
    return ESP_OK;
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
