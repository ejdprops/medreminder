# medreminder: project context for Claude Code

Medication reminder hardware + firmware. Devices are Matter-over-Thread accessories in Apple Home.
The repo holds the KiCad V1 master board (root) and the ESP-IDF/esp-matter firmware (`firmware/`).

Firmware development currently runs on a **Waveshare ESP32-C5-Touch-LCD-2.8** dev board.
Planned additional boards: Seeed XIAO ESP32-C6 or a Waveshare ESP32-H2 (undecided).

## How the user works

- Step by step: give one step, say how to tell it worked, wait for confirmation before the next.
- Peer tone, no filler. Recommend, don't list equal options.
- The user runs Claude Code on the Mac in this repo. Commands that need hands (phone pairing,
  pressing RESET/BOOT, plugging things in) go to the user; everything else you run yourself.

## Mac toolchain (already installed; do not reinstall)

| Thing | Where / version |
|---|---|
| ESP-IDF | `~/esp/esp-idf`, **v5.5.5** (required by esp-matter release/v1.5 for C5) |
| esp-matter | `~/esp/esp-matter`, branch **release/v1.5**, installed with `--no-host-tool` |
| Python for IDF | pinned to **3.12** via `~/esp/pybin/python3` (Homebrew default is 3.15, too new) |
| Env aliases (`~/.zshrc`) | `getidf` (IDF only), `getmatter` (IDF + esp-matter). ccache on (`IDF_CCACHE_ENABLE=1`) |
| Factory firmware backup | `~/esp/backups/c5_waveshare_factory_v1.0.0.bin` (full 32MB, restore with `write_flash 0x0`) |

Every build/flash command needs `getmatter` loaded in the shell first (start `claude` from such a shell).

## Build, flash, logs

```bash
cd firmware
./build.sh c5_waveshare                                      # build
./build.sh c5_waveshare -p /dev/cu.usbmodem1101 flash        # flash (keeps Matter pairing)
./build.sh c5_waveshare -p /dev/cu.usbmodem1101 erase-flash flash   # factory reset + flash
./build.sh c5_waveshare fresh                                # regenerate sdkconfig after editing defaults
python tools/serial_capture.py --seconds 60 --reset --out /tmp/boot.log   # read logs (non-interactive)
```

- `build.sh` gives each board its own `build/<board>/` with its `sdkconfig` inside. Config sources are
  `sdkconfig.defaults` + `boards/<board>/sdkconfig.defaults.<NET>` (NET=thread default) +
  `boards/<board>/sdkconfig.defaults`. Edit those, never the generated sdkconfig. After editing
  defaults, use `fresh`.
- **Do not use `idf.py monitor`** (interactive, never exits). Use `tools/serial_capture.py`.
  Close any monitor the user has open first; only one process can hold the port.
- Serial port: `/dev/cu.usbmodem1101` (C5 native USB-Serial/JTAG). Console **input** is on UART
  GPIO11/12, not USB. Do not set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: with the Matter shell enabled
  it spins redrawing the prompt, starves the CPU and breaks commissioning (tested).
- First Matter build is ~15-25 min; incremental builds are fast.

## Firmware layout

```
firmware/
├── CMakeLists.txt        reads BOARD (-DBOARD / env, default c5_waveshare); sets ESP_MATTER_DEVICE_PATH itself
├── build.sh
├── sdkconfig.defaults    common (from esp-matter light example)
├── main/                 app code: still esp-matter's light example (placeholder Matter device)
├── tools/serial_capture.py
└── boards/c5_waveshare/
    ├── target                     esp32c5
    ├── esp_matter_device.cmake    led_type ws2812, button_type iot
    ├── sdkconfig.defaults         32MB flash, PSRAM 80MHz, custom partitions, coredump to flash
    ├── sdkconfig.defaults.thread  Thread-only Matter config
    ├── partitions.csv
    └── components/device/         pin map for esp-matter's LED/button HAL
```

New board = new folder under `boards/` with the same files. Nothing in `~/esp/esp-matter` is modified
for this project (an old `device_hal/device/waveshare_c5_lcd28/` there is a leftover; ignore it).

## Waveshare ESP32-C5-Touch-LCD-2.8 facts

Chip ESP32-C5HR8 rev 1.2, single core 240MHz, **8MB PSRAM in package**, **32MB external flash**,
Wi-Fi 6 dual band, BLE 5, 802.15.4 (Thread). Pin map from Waveshare's BSP
(fork: `ejdprops/ESP32-C5-Touch-LCD-2.8`, `examples/esp-idf/*/components/esp32_c5_touch_lcd_2_8`):

| Function | GPIO |
|---|---|
| I2C SDA / SCL (touch, IO expander, IMU QMI8658, RTC PCF85063, SHTC3) | 0 / 1 |
| Touch INT (CST3530, I2C 0x58) | 5 |
| LCD SPI MOSI / SCLK (ST7789, 240×320 portrait) | 7 / 6 |
| LCD DC / CS | 9 / 10 |
| SD card MISO / CS (shares LCD MOSI/SCLK on SPI2) | 8 / 23 |
| I2S BCLK / LRCLK / DOUT / DIN (ES8311 codec, ES7210 mic ADC) | 24 / 25 / 26 / 27 |
| BOOT button (active low) | 28 |
| Console UART | 11 / 12 |

- **Never drive GPIO27**: it is the codec's output to the MCU. The light example's LED was moved to
  GPIO26 (MCU-driven codec input, harmless; there is no physical LED).
- LCD reset, touch reset, backlight PWM and speaker amp enable are on a **CH32V003 IO expander at I2C
  0x24** (EXIO0 touch reset, EXIO1 LCD reset, EXIO3 amp enable). The screen stays dark until firmware
  talks to it.
- Partition table (`boards/c5_waveshare/partitions.csv`): offsets up to 0x20000 match esp-matter's
  default so NVS (pairing) survives reflashing; ota_0/ota_1 8MB each; fctry 24K; coredump 64K;
  storage ~15.75MB (spiffs subtype) for UI assets and alert sounds.

## Matter / Apple Home status

- esp-matter v1.5 (Matter 1.5), Thread only (`ENABLE_WIFI_STATION` off), FTD (becomes a Thread router).
- Test onboarding codes: manual `3497-011-2332`, QR `MT:Y.K9042C00KA0648G00`.
- Home: HomePod mini must be the **active home hub** (automatic hub selection turned off); with the
  Apple TV active, iOS reported "needs a Thread border router" and never sent Thread credentials.
- Two Apple border routers on one network `MyHome1580347441` ("Kitchen", "Living Room").
- **Open issue:** pairing completes the Apple Home fabric (VendorId 0x1349) but the follow-up
  Apple Keychain fabric (0x1384) often times out (`Fail-safe timer expired`, then Home sends
  `RemoveFabric`). Root cause is the iPhone (iPhone Air, iOS 27.2) not discovering Thread devices on
  the LAN: Eve's Thread view shows no border routers while the Mac sees both
  (`dns-sd -B _meshcop._udp`). It is not a firmware bug. Pairing worked once fully.
  Pairing is not needed for UI/audio work.
- Harmless log noise: `chip[DIS] ... Failed to advertise ...: 3` before joining Thread,
  `Failed to process UDP - Duplicated` (two border routers), unknown-cluster read of `0x1349_FC00`
  (Apple-only cluster), `Long dispatch time`, SEC_CERT_DAC_PROVIDER / unknown kconfig symbol warnings,
  legacy RMT driver warnings.

## UI plan (next work)

- **Plain LVGL** (not ESP-Brookesia: experimental, phone-launcher model, unverified with the
  C++17 Matter forces). Reuse Waveshare's look: icon images (112×112 ARGB8888 C arrays, e.g.
  `examples/factory/components/Settings/assets/img_app_settings.c`), dark stylesheet colors/fonts
  (`examples/factory/main/dark/`), LVGL QR widget usage from their Settings app.
- Design: claude.ai artifact "MedReminder Round Display UI", artboard
  **"Living room — C5 2.8in (320×240 landscape)"**. Landscape 320×240: top line date + "3/5 taken"
  progress; center 168px circle with per-dose pills (taken green, next amber, later blue, overdue red
  pulsing); four 56×56 corner tiles (packages TL, garage TR, couch lights BL opening a 2-light panel,
  front door BR); bottom-center node-sync status + **settings gear** (36px drawn, extend touch area to
  ≥44px). Gear opens Settings: Matter setup QR + manual code (generate from esp-matter onboarding
  codes, show until paired), pairing status, alert volume, brightness.
- Roadmap:
  - **7a** add Waveshare BSP (display, touch, CH32V003 expander) and show a static screen while
    Matter runs. Riskiest step: internal RAM is ~173KB free with Matter; LVGL/SPI DMA and I2S need
    internal buffers. Adopt Waveshare's PSRAM settings (`SPIRAM_FETCH_INSTRUCTIONS`,
    `SPIRAM_RODATA`, `SPIRAM_XIP_FROM_PSRAM`) to free RAM and keep the display alive during flash
    writes. Measure free internal heap at each step.
  - **7b** main screen per design + settings page with Matter QR.
  - **7c** overdue alert sound: ES8311 via Waveshare `bsp_extra` (`bsp_extra_i2s_write`), short WAV
    files in the `storage` partition, amp enabled via EXIO3. No MP3 decoder. Speaker 4–8Ω.
  - **7d** (later) background images from SD (JPEG via esp_new_jpeg into PSRAM; SD shares the LCD SPI
    bus, so load once) or from `storage`.
- Licensing: Waveshare code is Apache-2.0. Keep headers on copied files and list them in a
  `THIRD_PARTY_LICENSES` file. `esp_xiaozhi` has its own license; don't copy it.

## Gotchas

- Root `.gitignore` ignores `*.png`; force-add or adjust before committing PNG icon sources.
- KiCad V1 board has an ESP-12F (ESP8266) next to the XIAO C6: not supported by ESP-IDF and
  conflicts with the no-Wi-Fi device design; open question whether it stays for V2.
