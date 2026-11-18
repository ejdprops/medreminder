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

## Firmware versions

- `firmware/CMakeLists.txt`: `PROJECT_VER` (semver, Matter SoftwareVersionString) and
  `PROJECT_VER_NUMBER` (integer Matter SoftwareVersion; must increase every release, OTA compares
  it). Bump both together. Releases are tagged `fw-v<PROJECT_VER>`.
- Every build runs `tools/build_info.cmake` -> `build_info.h` with `git describe --tags --match
  "fw-v*" --dirty`. Screens (home bottom-center, settings bottom-right) show `v0.3.0` when built
  exactly at the tag with a clean tree; past it, commits since the tag and `*` for uncommitted
  changes (`v0.3.0-2*`); `v0.3.0-dev` if the latest tag isn't this PROJECT_VER. The boot log prints
  the full describe string and SoftwareVersion: `firmware v0.3.0-2* (fw-v0.3.0-2-gabc1234-dirty), ...`.
- Release: bump both values, commit, `git tag -a fw-vX.Y.Z -m "..."`, `git push && git push origin
  fw-vX.Y.Z`, then build and flash from the clean tagged tree. Version bumps change a Matter GN
  arg, so that build is slower; NVS (pairing) is kept.

## Firmware layout

```
firmware/
├── CMakeLists.txt        reads BOARD (-DBOARD / env, default c5_waveshare); sets ESP_MATTER_DEVICE_PATH itself
├── build.sh
├── sdkconfig.defaults    common (from esp-matter light example)
├── main/                 app_main.cpp (esp-matter light example, placeholder Matter device), ui.c
│   │                     (display startup), ui_home.c (home screen), icons/ (generated A8 images)
│   ├── icons/svg/        icon sources; regenerate with tools/svg_icons.py (see below)
│   └── fonts/            generated Inter LVGL fonts (tools/gen_fonts.sh) + OFL license
├── tools/serial_capture.py, tools/svg_icons.py, tools/gen_fonts.sh
├── THIRD_PARTY_LICENSES  copied third-party code (Waveshare BSP, ...)
└── boards/c5_waveshare/
    ├── target                     esp32c5
    ├── esp_matter_device.cmake    led_type hollow_led (no-op), button_type iot
    ├── sdkconfig.defaults         32MB flash, PSRAM 80MHz, custom partitions, coredump to flash
    ├── sdkconfig.defaults.thread  Thread-only Matter config
    ├── partitions.csv
    └── components/
        ├── device/                       pin map for esp-matter's LED/button HAL
        ├── esp32_c5_touch_lcd_2_8/       Waveshare BSP (trimmed; panel/touch rotation added)
        └── waveshare__esp_lcd_touch_cst3530/
```

- UI startup: `ui_start()` (after `esp_matter::start`) spawns a one-shot `ui_init` task with an 8KB
  PSRAM stack. `bsp_display_show()` renders the first frame on the calling task, which overflowed
  main's 3.5KB stack (boot loop, "Stack protection fault" in task main). LVGL's own task stack is
  in PSRAM too. UI code is guarded with `__has_include` so display-less boards still build.
- Landscape: `ROTATE_90` with the USB port on the right. The LVGL adapter does not rotate SPI
  panels; the BSP sets ST7789 swap/mirror and the matching CST3530 touch flags.
- Icons: 24x24 stroke SVGs (from the design artboard) -> 20px A8 C arrays, colored with
  `image_recolor`. Needs Pillow + svg.path in a throwaway venv (not the IDF env):
  `python -m venv /tmp/v && /tmp/v/bin/pip install pillow svg.path &&
  /tmp/v/bin/python tools/svg_icons.py --size 20 main/icons/svg/*.svg --out main/icons`.

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

- **Never drive GPIO27**: it is the codec's output to the MCU. GPIO26 is I2S DOUT (MCU to codec):
  the light example's LED uses the no-op `hollow_led` driver (there is no physical LED).
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
  `RemoveFabric`). It is not a firmware bug. Pairing worked once fully.
  Pairing is not needed for UI/audio work.
  - 2026-11-16 (after 7a): Home fabric committed, CASE for it came from a LAN host (likely the
    HomePod hub); Keychain fabric got AddNOC, then no CASE Sigma1 ever arrived, fail-safe (30s)
    expired, Home sent `RemoveFabric`. Internal heap min 26KB, so not a memory problem.
  - From the Mac the device is fully reachable: `dns-sd -B _matterc._udp` finds it (SRP via the
    border routers), it resolves to its Thread OMR address (`fdb8:8f45:c1d4::/64`, routed via a
    border router), and `ping6` succeeds. So the hub and the Mac reach the device; only the iPhone's
    CASE never arrives. Suspect: the iPhone doesn't get the OMR route (RA/RIO) or LAN mDNS (Wi-Fi
    mesh/AP filtering, different band/SSID/VLAN).
  - **Fixed 2026-11-16:** on the iPhone, turned off the per-network "connectivity assist" setting
    for the home Wi-Fi and toggled Wi-Fi off/on. After that the iPhone saw `_matterc._udp` and
    `_meshcop._udp` (Discovery app) and could ping6 the device's OMR address (HE.NET Network
    Tools), and pairing completed both fabrics: Home 0x1349 and Keychain 0x1384 (CASE arrived ~1s
    after AddNOC). Both changes were made together, so which one fixed it is unknown. If pairing
    fails again, check those two first, then repeat the iPhone browse/ping test.
    Heap min during pairing with the display running: 26KB.
  - Eve's Thread view has never shown the network, even when pairing worked: it is not evidence.
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
  **"C5 2.8in rectangular"** (renamed from "Living room — C5 2.8in": the device may move rooms and
  more units may go elsewhere, so artboard and code names are location-neutral). Landscape 320×240:
  white background (Settings > Dark mode switches every page, see 7c.5). Top line 16px bold:
  weekday + date deep blue, 24 h time deep green ("TUE  17 NOV · 1312 hrs"). Doses: a scrollable
  "today" list (y 32-198, five rows fit), one solid row per dose in the dose's own color (30px
  high, radius 15; `main/ui_dose_row.c`): time ("0600"), name, period (+ weekday for weekly
  doses), all 14px; dark text on light colors. State: "· next", "· late", "Taken" (16px) + "✓",
  overdue pulses with a red border. Scrolled to the next dose; scrollbar gutter on the right.
  Tap a next/overdue row to take it, a taken row to undo; later rows ignore taps. Chosen after
  testing over the earlier rotating pills (in git history at v0.4.0). Boot splash: Averak Labs
  logo (cyberpunk recolor) for
  5 s (`SPLASH_MS` in `main/ui.c`), then a fade to home. No corner tiles
  (the old package/garage/lights/door tiles were removed; icons are in git history). Bottom:
  node-sync status centered with a faint green (synced) / red outline instead of a dot, version
  centered below it, **settings gear** far right (36px drawn, 44px touch area), dev reset button
  far left (Kconfig `MEDREMINDER_DEV_TOOLS`, default on; turn off for release builds). Taps count
  only when held >= 100ms (`main/ui_tap.c`, `UI_TAP_MIN_MS`). Center layout is per board:
  Kconfig `MEDREMINDER_UI_LAYOUT` RECT (C5) or ROUND (old circle layout, for round displays).
  Gear opens Settings: Matter setup QR + manual code (generate from esp-matter onboarding
  codes, show until paired), pairing status, alert volume, brightness.
- Roadmap:
  - **7a** (done) Waveshare BSP (display, touch, CH32V003 expander) in
    `boards/c5_waveshare/components/` (sensors/Wi-Fi stripped), static test screen from
    `main/ui.c`, portrait. PSRAM settings: XIP from PSRAM + `SPIRAM_MALLOC_ALWAYSINTERNAL=0`;
    LVGL task stack in PSRAM. `app_main` logs `heap[...]` every 10 s; internal RAM figures:
    | State | Free idle | Min ever |
    |---|---|---|
    | Matter only, defaults | 102.6KB | 63KB |
    | + PSRAM settings | 104.7KB | 72.5KB |
    | + display/touch/LVGL | 61.6KB | 29.3KB (during display init) |
    BSP costs ~12.5KB static IRAM/DRAM + 2×9.6KB internal DMA draw buffers. Not yet measured:
    commissioning with the display running. Spare levers: SPI ISR / ringbuf out of IRAM (~9KB,
    relies on XIP keeping cache on during flash writes), `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`,
    BLE shutdown after pairing.
  - **7b** main screen per design + settings page with Matter QR.
    - 7b.1 (done) landscape 320x240, touch verified at all four corners.
    - 7b.2 (done) home screen per artboard "C5 2.8in rectangular" with placeholder data
      (`main/ui_home.c`); internal RAM unchanged (62.9KB idle, min 29.5KB).
    - 7b.3 (done) project fonts: Inter 4.1 (OFL) via `tools/gen_fonts.sh <Inter ttf dir>` (Node `npx
      lv_font_conv@1.5.3`, Node from Homebrew) -> `main/fonts/` (committed). Sizes/weights from the
      design: 8, 9 regular; 9, 10, 11, 12, 14, 16, 23 semibold; 12, 16 bold. Glyphs: ASCII + "·" + "✓" only; add
      ranges in the script for new characters. Built-in Montserrat: only LVGL's default 14 left.
    - 7b.4 (done) settings page (`main/ui_settings.c`): Matter QR + manual code generated on the
      device (`main/matter_info.cpp`, takes the CHIP stack lock), hidden behind "Show setup code"
      when paired (re-hidden every visit), pairing status, live brightness, volume slider disabled
      until 7c. Version label bottom-right on both screens.
    - 7b.5 (done) live clock: SNTP over Thread (`main/time_sync.c`). The Apple border routers
      provide NAT64; `CONFIG_OPENTHREAD_DNS64_CLIENT=y` makes names resolve to NAT64 addresses.
      First sync ~2-3 s after start, hourly after. Kconfig "MedReminder": `MEDREMINDER_TZ`
      (POSIX, default US Central `CST6CDT,M3.2.0,M11.1.0`), `MEDREMINDER_NTP_SERVER`
      (`pool.ntp.org`). Costs ~2.5KB internal RAM (60.2KB idle, min 27.7KB).
    - 7b.6 (done) five-pill home screen (above), replacing the circle + corner tiles on the C5.
    - 7b.7 (done, v0.3.0) big center pill / small surrounding pills, centered text, 100ms tap
      minimum, single decaying click (a held tone with hard edges sounded like a double tick),
      dev reset button, bottom row rework, version label with commit count.
  - **7c** overdue alert sound: ES8311 via the BSP, short WAV files in the `storage` partition, amp
    enabled via EXIO3. No MP3 decoder. Speaker 4–8Ω (connected).
    - 7c.1 (done) `main/audio.c`: queue-fed audio task, `audio_play(AUDIO_CLICK | AUDIO_PREVIEW)`,
      volume 0-100 in NVS (`audio`/`volume`, default 60) via the settings slider (saved + preview
      beeps on release). Clicks on taps that change something: gear, Back, "Show setup code",
      tapping the center (next) pill marks it taken and rotates the pills (placeholder data, resets
      on reboot; consider undo by tapping the UR "most recent" pill).
      - BSP made speaker-only (no I2S RX, ES8311 DAC mode). The esp-matter LED is `hollow_led`:
        GPIO26 is I2S DOUT and must not be driven by the WS2812/RMT driver.
      - Codec + I2S + amp come up on the first sound and shut down 3 s after the last one, so
        idle internal RAM is unchanged (61.5KB idle, boot min ~29KB; ~3-6KB while sounding).
        `heap_caps_get_minimum_free_size` sums per-region lows, so "min" can drop after a sound
        even though free never went that low.
      - Amp (EXIO3) needs ~150ms after enable before sound is heard (`AMP_SETTLE_MS`); toggling
        it per sound swallowed short clicks. Writes return when samples are queued in the 90ms
        I2S DMA buffer, so push `DMA_FLUSH_MS` of silence before muting.
      - Audio must start from the BSP init task (`ui.c` calls `audio_start()` after display start):
        BSP init functions are not thread-safe.
      - Tones: click 2kHz decaying 25ms; alert/preview C6-E6-G6 1047/1319/1568Hz, 70% level. First
        click after idle lags ~0.2s (amp start).
    - 7c.2 (done, v0.4.0) overdue alerts (`main/doses.c`): placeholder schedule Evan AM 6:00, Karen AM
      9:00, MID 13:30, PM 19:00, BED 21:00. Overdue 30 min after the time: pill red + pulsing,
      center shows "late", alert chime now and every 5 min until taken. Taken flags in NVS
      (`doses`/`date`+`taken`), reset at midnight. Dev reset button clears them.
      - Kconfig `MEDREMINDER_DOSE_TEST` (default off): first dose due at clock set, then every
        2 min, overdue after 1 min, repeat every minute; saves to NVS namespace `doses_test`.
      - Sounds are pre-rendered into PSRAM at audio start (~40KB). Rendering with sinf in real
        time crackled while a pill pulsed: LVGL (priority 6) redraws nonstop and the audio task
        (5) fell behind the 90ms DMA buffer. It was not the speaker: clean at 100% on Settings.
      - `esp_codec_dev_write` applies software volume **in place**: write a copy, never a stored
        buffer (it scaled the stored sound down on every play until silent).
      - The ES8311/amp keep their state across flash and reset. Once all sound stopped and only a
        USB power cycle brought it back: if audio dies with no errors in the log, power-cycle
        first.
      - 24kHz made the crackle worse (more samples to compute); 16kHz is fine.
    - 7c.3 (done, v0.4.0) About screen (`main/ui_about.c`, Settings > About): Averak Labs neon logo
      (`main/images/src/averak_labs_neon.png`, from ~/AverakLabs; converted by
      `tools/png_image.py` to RGB565 116x168, screen bg = logo bg #0b0d14), product/vendor from
      Matter Basic Information, firmware, Matter VID/PID, hardware, uptime, licenses.
      Matter names come from `boards/<board>/CHIPProjectConfig.h` via `CONFIG_CHIP_PROJECT_CONFIG`
      (C5: "Averak Labs" / "Pill Reminder Rect"; a round board would use "Pill Reminder Round").
      Changing it rebuilds Matter (~20 min). Apple Home may need the accessory re-added to show
      new names.
    - 7c.4 (done, v0.5.0) dose schedule editing + undo. Settings > Doses (`main/ui_doses.c`): scrollable list
      (time, name, period), tap a row to edit, "+ Add"; edit screen has name (LVGL keyboard),
      period AM/MID/PM/BED, hour/5-min/AM-PM rollers, Save, Delete (tap twice). Up to 16 doses
      (`DOSE_MAX`), saved as an NVS blob (`doses`/`sched`), sorted by time. New setup (nothing
      saved): defaults "Dose" AM 7:00, MID 12:00, PM 18:00, BED 21:00. Rule: one
      person can't have two doses at the same time (`doses_set` returns the error shown).
      Editing keeps today's taken flags on unchanged doses.
      - Home: tap the UR (most recently taken) pill to undo. More than 5 doses: the pills are
        replaced by a scrollable "today" list of pill-colored rows scrolled to the next dose; tap a
        next/overdue row to take, a taken row to undo, later rows ignore taps. (Stacked period
        pills were tried and rejected: hard to read.)
      - One-line LVGL text areas jitter if the box is even 1px shorter than border + padding +
        font line height (it scrolls as the cursor blinks).
    - 7c.5 (done, v0.6.0) dose colors, weekly doses, themes, 24 h:
      - Per-dose color from 12 (`dose_colors`, index saved per dose, append only; picker in
        spectrum order `dose_color_order`), 3x4 swatches in the editor + a live sample in the
        header. Rows are solid (an opacity option was tried and removed; `opacity` stays in the
        struct for the saved layout).
      - Weekly doses: `days` bitmask (Daily or one weekday roller); only shown/alerted on their
        day; same person + same time allowed on different days. Schedule NVS key `sched2`
        (`dose_def_t` with color/opacity/days, 32 bytes); old `sched` is migrated on load.
      - Light/dark theme (`main/ui_theme.c`, NVS `ui`/`dark`, light default): every screen takes
        colors from `UI_T`; the Settings switch rebuilds home in place (`ui_home_restyle`), drops
        cached Doses/About screens and rebuilds Settings, no reboot.
      - 24 h military time everywhere ("0600"; clock "1312 hrs"); editor hour roller 00-23.
      - Scrollbar gutter (`main/ui_scroll.c`): 32px touch strip right of the list with a track +
        thumb; touch jumps/drags (down = down), so scrolling there never takes a dose. Lists
        stop before it. LVGL list padding is not touchable (plain objects aren't clickable), so a
        padding-only gutter did nothing.
      - Rows have one font size (per-row auto-fit looked uneven). A label with LONG_DOT needs a
        fixed height or it wraps instead of truncating.
      - Logo: `tools/cyber_logo.py` recolors `averak_labs_neon.png` -> `averak_labs_cyber.png`
        (yellow AVERAK, glowing magenta strokes); About shows it in a dark tile.

- Licensing: Waveshare code is Apache-2.0. Keep headers on copied files and list them in a
  `THIRD_PARTY_LICENSES` file. `esp_xiaozhi` has its own license; don't copy it.

## Gotchas

- Root `.gitignore` ignores `*.png`; force-add or adjust before committing PNG icon sources.
- KiCad V1 board has an ESP-12F (ESP8266) next to the XIAO C6: not supported by ESP-IDF and
  conflicts with the no-Wi-Fi device design; open question whether it stays for V2.
