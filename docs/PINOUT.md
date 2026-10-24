# PINOUT.md — KAST Hardware Pin Assignments (The Law)

> **Board:** JCZN / Guition ESP32-4848S040 (SKU family ESP32-4848S040C_I) —
> 4.0" 480×480 ESP32-S3-N16R8 smart display
> **Official vendor package:** `docs\schematics\4.0inch_ESP32-4848S040\`
> (downloaded from the manufacturer drive `pan.jczn1688.com` — present ✅)
>
> **Source status: VENDOR-CONFIRMED (2026-10-21).** Every row below is now
> confirmed by the manufacturer's own materials:
> - `5-IO pin distribution\4.0 inches IO pin distribution.xlsx` — official pin map
> - `1-Demo\Demo_Arduino\1_2_4.0_LvglWidgets` — official LCD init (pins, BGR, timings)
> - `1-Demo\Demo_Arduino\1_3_...music\player.h` — official I2S pins (DOUT=40, BCLK=1, LRC=2)
> - `1-Demo\Demo_Arduino\1_1_86switch_onoff\touch.h` — official GT911 config (INT/RST = -1)
> - `4-Driver_IC_Data_Sheet\Nsiway-NS4168.pdf` — speaker amp is the NS4168 (I2S Class-D)
> - `2-Specification\ESP32-4848S040 Specifications-EN.pdf` — module specs
>
> The earlier community cross-check (Tasmota/ESPHome/HA boot log) matched the
> vendor data on every point — recorded in PROGRESS.md as verification evidence.

---

## 1. LCD — Sitronix ST7701, 480×480 RGB565

RGB parallel bus via the ESP32-S3 LCD peripheral, plus a 3-wire SPI (9-bit)
command channel. **Not** an SPI display.

### 1.1 RGB data bus (vendor demo pin order)

| Signal | GPIO | Notes |
|--------|------|-------|
| R0–R4 | **11, 12, 13, 14, 0** | R4 = GPIO0, a BOOT strapping pin — gotcha K-1 |
| G0–G5 | **8, 20, 3, 46, 9, 10** | G1 = GPIO20 (USB D+, consumed); G2 = GPIO3; G3 = GPIO46 |
| B0–B4 | **4, 5, 6, 7, 15** | |
| Color order | — | **BGR** (vendor init: `BGR=true`), IPS panel |

### 1.2 RGB control lines

| Signal | GPIO | Notes |
|--------|------|-------|
| DE / VSYNC / HSYNC / PCLK | **18 / 17 / 16 / 21** | ~12 MHz PCLK nominal |
| Backlight | **38** | LEDC PWM, `GFX_BL 38` in vendor demo |

### 1.3 Command SPI (3-wire / 9-bit, send-only)

| Signal | GPIO | Notes |
|--------|------|-------|
| CS / SCL / SDA | **39 / 48 / 47** | 48/47 shared with SD card |

### 1.4 Timings — **VENDOR OFFICIAL (wins)**

| Param | Value | Community variants (superseded) |
|-------|-------|--------------------------------|
| hsync_front_porch | **10** | ESPHome: 10 |
| hsync_pulse_width | **8** | ESPHome: 8 · HA log: 10 |
| hsync_back_porch | **50** | ESPHome: 20 · HA log: 10 |
| vsync_front_porch | **10** | 10 |
| vsync_pulse_width | **8** | ESPHome: 8 · HA log: 10 |
| vsync_back_porch | **20** | 10 |

Init sequence: use `st7701_type1_init_operations` (vendor demo / Arduino_GFX),
which includes the required `0xFF 0x77…` bank select; ESPHome adds `0xCD 0x00`
(MDT flag off) — keep both.

---

## 2. Touch — Goodix GT911 (I2C) — vendor touch.h

| Signal | GPIO / Value | Notes |
|--------|--------------|-------|
| SDA / SCL | **19 / 45** | 19 = USB D- (consumed); 45 = strapping pin |
| INT / RST | **not connected** (`-1` in vendor demo) | driver must skip reset sequence |
| Address | **0x5D** | |
| Coordinate map | vendor maps raw 480→0 on both axes | check raw coords at bring-up; apply mirror/swap only if needed |

---

## 3. SD card (TF) slot — SDSPI

| Signal | GPIO | Notes |
|--------|------|-------|
| CS / MISO / SCK / MOSI | **42 / 41 / 48 / 47** | SCK+MOSI shared with LCD cmd SPI — gotcha K-3 |

Proven mount: SDSPI host, 20 MHz. Uses: chat log, emote cache, SFX clips.
Speaker audio in the vendor music demo also streams from SD (`ui_img_sdcard_icon`).

---

## 4. Speaker — I2S → NS4168 Class-D amp (MX1.25 2P "Speak") ✅ CONFIRMED

| Signal | GPIO | Source |
|--------|------|--------|
| I2S DOUT | **40** | vendor `player.h` (`I2S_DOUT 40`) |
| I2S BCLK | **1** | vendor `player.h` (`I2S_BCLK 1`) |
| I2S LRCK | **2** | vendor `player.h` (`I2S_LRC 2`) |
| Amp | **NS4168** (Nsiway) | vendor datasheet in `4-Driver_IC_Data_Sheet\` |

⚠️ Note: some community configs use GPIO 1/2/40 for relays — that is the
**relay-stuffed SKU** (ESP32-4848S040C_I_Y_1/_Y_3). Ours is the speaker/audio
stuffing; vendor audio demo confirms these pins drive the amp path.

---

## 5. USB & serial

| Item | Detail |
|------|--------|
| USB-C port | **CH340-class USB-UART bridge** → main UART (IO43 TX / IO44 RX) — NOT native USB |
| Flash/monitor | CH340 COM port — use a **USB-A → USB-C cable** (C-to-C e-marker won't enumerate) |
| Native USB (GPIO19/20) | **consumed** (touch SDA / LCD G1) → **no USB HID possible** (D-5, gotcha K-2) |
| UART1 (IO17/18) | alternate function of VSYNC/DE lines — 4P expansion probable; not needed for KAST |

---

## 6. Power & other

| Item | Detail |
|------|--------|
| Power in | USB-C, 5 V, ~260 mA typical (official spec) |
| BAT terminal (MX1.25 2P) | present; charge/PMIC IC not documented in vendor package — battery stays v2 scope |
| Battery fuel gauge | none → no battery icon in status bar (D-… recorded in ROADMAP backlog) |
| SW1 | hidden momentary button beside USB — use if download mode fails (hold while plugging in) |

## Gotchas

- **K-1:** strapping pins GPIO0 (R4), GPIO46 (G3), GPIO45 (touch SCL), GPIO3 (G2)
  on the RGB/touch bus — flash via `idf.py` (DTR/RTS), hold panel in reset until
  clocks stable; SW1 recovery if download mode fails.
- **K-2:** native USB consumed → USB HID impossible; OBS-WS only.
- **K-3:** SD shares SCK/MOSI with the LCD command SPI — use ESP-IDF SDSPI
  (dedicated host, 20 MHz), never bit-bang.

## Rules

1. This file is vendor-confirmed; the vendor package in `docs\schematics\` is
   the raw source and wins on any conflict (update both).
2. The compile-time pin header must match this file exactly.
3. Nothing here is guessed or single-source anymore — the 2026-10-17 ⚠️ VERIFY
   items are closed by the vendor package.
