/*
 * pins.h — KAST compile-time pin macros.
 * Generated from docs/PINOUT.md (the Law). Every GPIO here must match
 * PINOUT.md exactly; PINOUT.md wins on any conflict.
 *
 * Board: JCZN/Guition ESP32-4848S040 — ESP32-S3-N16R8, ST7701S RGB + GT911.
 * All values vendor-confirmed from the official package (2026-10-21).
 */
#pragma once

/* ---- ST7701S RGB display: 16-bit data bus ---- */
#define KAST_LCD_R0        11
#define KAST_LCD_R1        12
#define KAST_LCD_R2        13
#define KAST_LCD_R3        14
#define KAST_LCD_R4        0   /* BOOT strapping pin — gotcha K-1 */
#define KAST_LCD_G0        8
#define KAST_LCD_G1        20  /* native USB D+ (consumed) */
#define KAST_LCD_G2        3   /* JTAG/strapping */
#define KAST_LCD_G3        46  /* strapping */
#define KAST_LCD_G4        9
#define KAST_LCD_G5        10
#define KAST_LCD_B0        4
#define KAST_LCD_B1        5
#define KAST_LCD_B2        6
#define KAST_LCD_B3        7
#define KAST_LCD_B4        15

/* ---- RGB control ---- */
#define KAST_LCD_DE        18
#define KAST_LCD_VSYNC     17
#define KAST_LCD_HSYNC     16
#define KAST_LCD_PCLK      21
#define KAST_LCD_BL        38  /* backlight, LEDC PWM */

/* Vendor timing table (PINOUT.md §1.4 — the Law) */
#define KAST_LCD_HFP       10
#define KAST_LCD_HPW       8
#define KAST_LCD_HBP       50
#define KAST_LCD_VFP       10
#define KAST_LCD_VPW       8
#define KAST_LCD_VBP       20
#define KAST_LCD_PCLK_HZ   (12 * 1000 * 1000)

/* ---- 3-wire SPI command channel (shared with SD) ---- */
#define KAST_LCD_CS        39
#define KAST_LCD_SCL       48  /* shared: SD SCK */
#define KAST_LCD_SDA       47  /* shared: SD MOSI */

/* ---- GT911 touch (I2C, addr 0x5D, no INT/RST) ---- */
#define KAST_TOUCH_SDA     19  /* native USB D- (consumed) */
#define KAST_TOUCH_SCL     45  /* strapping pin — gotcha K-1 */
#define KAST_TOUCH_ADDR    0x5D

/* ---- SD card (SDSPI) ---- */
#define KAST_SD_CS         42
#define KAST_SD_MISO       41
#define KAST_SD_SCK        48  /* shared with LCD cmd SPI */
#define KAST_SD_MOSI       47  /* shared with LCD cmd SPI */

/* ---- I2S audio -> NS4168 amp (MX1.25 2P "Speak") ---- */
#define KAST_I2S_BCLK      1
#define KAST_I2S_LRCK      2
#define KAST_I2S_DOUT      40
