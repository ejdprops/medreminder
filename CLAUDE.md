# medreminder: project context for Claude Code

Medication reminder hardware + firmware. Devices are Matter-over-Thread accessories in Apple Home.
The repo holds the KiCad V1 master board (root) and the ESP-IDF/esp-matter firmware (`firmware/`).

Firmware runs on two dev boards, both Matter-over-Thread:
- **Main unit:** Waveshare ESP32-C5-Touch-LCD-2.8 (`boards/c5_waveshare`): schedule editing, alert
  sound, dose accessories in Apple Home.
- **Node:** Waveshare ESP32-C6-LCD-1.28 (`boards/c6_round`; the user's board is a kctozo clone with
  that silkscreen): round 240x240, **no touch**, shows the doses (see 7g). More nodes may follow.
- **Dashboard node:** Seeed XIAO ESP32-C6 (`boards/xiao_c6_dash`), no display: a node that feeds a
  CrowPanel 7" ESP32-S3 dashboard over one wire (see 7h). The S3 dashboard is a separate IDF
  project (not in this repo, no Matter).

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
./build.sh c6_round ...                                      # same for the node (one board plugged in at a time)
python tools/serial_capture.py --seconds 60 --reset --out /tmp/boot.log   # read logs (non-interactive)
```

- `build.sh` gives each board its own `build/<board>/` with its `sdkconfig` inside. Config sources are
  `sdkconfig.defaults` + `boards/<board>/sdkconfig.defaults.<NET>` (NET=thread default) +
  `boards/<board>/sdkconfig.defaults`. Edit those, never the generated sdkconfig. After editing
  defaults, use `fresh`.
- One component lock per board: `boards/<board>/dependencies.lock` (set in `firmware/CMakeLists.txt`
  via `DEPENDENCIES_LOCK`); `managed_components/` is shared.
- **Do not use `idf.py monitor`** (interactive, never exits). Use `tools/serial_capture.py`.
  Close any monitor the user has open first; only one process can hold the port.
- Serial port: `/dev/cu.usbmodem1101` (native USB-Serial/JTAG; the C5 and the C6 both show up
  there, check with `esptool.py chip_id` which one is plugged in). On the C5, console **input** is
  on UART GPIO11/12, not USB. Do not set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: with the Matter shell enabled
  it spins redrawing the prompt, starves the CPU and breaks commissioning (tested).
- First Matter build is ~15-25 min; incremental builds are fast.
- `serial_capture.py --reset` on the C6 can lose the log after the bootloader (USB re-enumerates);
  capture again without `--reset`. Opening the C6's port sometimes restarts it too (and the first
  ~30 s of log are lost), so start a long capture before asking the user to do something. Every
  `heap[...]` line (10 s) ends with the last reset's cause (`reset usb`, `power-on`, `panic`...).

## Firmware versions

- `firmware/CMakeLists.txt`: `PROJECT_VER` (semver, Matter SoftwareVersionString) and
  `PROJECT_VER_NUMBER` (integer Matter SoftwareVersion; must increase every release, OTA compares
  it). Bump both together. Releases are tagged `fw-v<PROJECT_VER>`.
- Every build runs `tools/build_info.cmake` -> `build_info.h` with `git describe --tags --match
  "fw-v*" --dirty`. Screens (home bottom-center, settings bottom-left) show `v0.3.0` when built
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
├── main/                 app_main.cpp (Matter node), matter_dev.cpp (bridge: dose accessories), ui.c
│   │                     (display startup), ui_home.c (RECT home), ui_round.c (ROUND home, node),
│   │                     ui_settings.c, ui_doses.c (schedule list + editor), ui_sleep.c,
│   │                     node_sync.c (C5 <-> nodes over Thread), imu_shake.c (shake the node),
│   │                     dose_colors.c (palette, also built by dashboard/),
│   │                     uart_link.c (dashboard node <-> CrowPanel, one wire),
│   │                     mem_caps.h (PSRAM or internal), icons/ (generated A8 images)
│   ├── icons/svg/        icon sources; regenerate with tools/svg_icons.py (see below)
│   └── fonts/            generated Inter LVGL fonts (tools/gen_fonts.sh) + OFL license
├── tools/serial_capture.py, tools/svg_icons.py, tools/gen_fonts.sh
├── THIRD_PARTY_LICENSES  copied third-party code (Waveshare BSP, ...)
├── boards/c5_waveshare/
│   ├── target                     esp32c5
│   ├── esp_matter_device.cmake    led_type hollow_led (no-op), button_type iot
│   ├── sdkconfig.defaults         32MB flash, PSRAM 80MHz, custom partitions, coredump to flash
│   ├── sdkconfig.defaults.thread  Thread-only Matter config
│   ├── partitions.csv, CHIPProjectConfig.h, dependencies.lock
│   └── components/
│       ├── device/                       pin map for esp-matter's LED/button HAL
│       ├── esp32_c5_touch_lcd_2_8/       Waveshare BSP (trimmed; panel/touch rotation added)
│       └── waveshare__esp_lcd_touch_cst3530/
├── boards/c6_round/               same files; 16MB flash, no PSRAM, ROLE_NODE + LAYOUT_ROUND
│   └── components/
│       ├── device/                       BOOT GPIO9, no LED
│       └── esp32_c6_lcd_1_28/            BSP rewritten from Waveshare's: display, backlight, I2C only
└── boards/xiao_c6_dash/           4MB flash (no storage partition), ROLE_NODE, no BSP (headless),
    └── components/device/            UART_LINK_GPIO 2; BOOT GPIO9, LED unused
```

- UI startup: `ui_start()` (after `esp_matter::start`) spawns a one-shot `ui_init` task with an 8KB
  PSRAM stack. `bsp_display_show()` renders the first frame on the calling task, which overflowed
  main's 3.5KB stack (boot loop, "Stack protection fault" in task main). LVGL's own task stack is
  in PSRAM too. UI code is guarded with `__has_include` so display-less boards still build.
- Per board, from the BSP header: `BSP_LCD_ROTATION` (C5 `ROTATE_90`, C6 `ROTATE_0`) and
  `BSP_CAPS_AUDIO` (audio.c builds only with it). Task stacks and big buffers use `MEM_CAPS_BULK`
  (`main/mem_caps.h`): PSRAM if the board has it, else internal.
- Boards without a display BSP (no `bsp/esp-bsp.h`, so no LVGL; every UI file is guarded with
  `__has_include("lvgl.h")`): `ui_start()` instead starts a `doses` task that calls `doses_init()`
  and `doses_tick()` every second (node sync, Home requests, midnight).
- Kconfig `MEDREMINDER_ROLE`: MAIN (C5) or NODE (C6: `matter_dev` keeps the empty aggregator and
  adds no dose endpoints, so Home doesn't get duplicates).
- C5 landscape: `ROTATE_90` with the USB port on the right. The LVGL adapter does not rotate SPI
  panels; the BSP sets ST7789 swap/mirror and the matching CST3530 touch flags.
- Icons: 24x24 stroke SVGs (from the design artboard) -> 20px A8 C arrays, colored with
  `image_recolor`. Needs Pillow + svg.path in a throwaway venv (not the IDF env):
  `python -m venv /tmp/v && /tmp/v/bin/pip install pillow svg.path &&
  /tmp/v/bin/python tools/svg_icons.py --size 20 main/icons/svg/*.svg --out main/icons`.
  New icons need a `LV_IMAGE_DECLARE` in `main/icons/icons.h`; new source files need
  `./build.sh <board> reconfigure build` (main globs its source dirs).

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

## Waveshare ESP32-C6-LCD-1.28 facts (node)

ESP32-C6 QFN40 rev 0.2, single core 160MHz, 512KB SRAM, **no PSRAM**, 16MB flash. Pin map from
`waveshareteam/ESP32-C6-LCD-1.28` HARDWARE_REFERENCE.md (same pins as the -Touch- variant minus touch):

| Function | GPIO |
|---|---|
| LCD GC9A01 SPI2 MOSI / SCLK / CS / DC / RST | 21 / 11 / 10 / 19 / 20 |
| LCD backlight (LEDC PWM) | 7 |
| I2C SDA / SCL (QMI8658 IMU 0x6B, PCF85063 RTC 0x51) | 18 / 8 |
| IMU INT1 / INT2, RTC INT | 5 / 4, 6 |
| TF card CS / MISO (shares LCD SPI) | 15 / 22 |
| Battery ADC (200K/100K divider) | 0 |
| BOOT button | 9 |
| UART TX / RX | 16 / 17 |

- **GPIO23 is the charger status line on this board.** Waveshare's BSP (and even their non-touch
  demo) drives it as touch reset; ours leaves touch out.
- No speaker/codec: the node alerts by screen only (a buzzer on GPIO1-3 is possible later).
- Input: no touch; BOOT button and the QMI8658 IMU (shake to take, `main/imu_shake.c`; was a
  double tap until 2026-11-20).
  **The QMI8658's built-in tap engine never reported a tap on this board** (configured per
  SensorLib's `configTap`, registers read back correctly, STATUS1/TAP_STATUS stayed 0): taps are
  detected in software from the FIFO (500 Hz, every sample). Lying still, sample-to-sample noise is
  ~3 mg; a finger tap is a 150-400 mg jump. The first FIFO read after power-up holds junk
  (-32768s). Picking the node up while on USB restarted it once (no coredump: likely the plug).
- RAM: idle internal free ~175KB with display + LVGL + Matter (no dose endpoints), 150KB with them.
  Static DIRAM ~210KB.
- **LVGL (priority 6) outranks the CHIP task (priority 1) on one core:** anything redrawing every
  frame starves Matter (task watchdog on `lvgl`/`swdraw`; pairing timed out). The overdue ring
  blinks on a 700ms timer instead of fading; the pairing screens are static.

## Seeed XIAO ESP32-C6 facts (dashboard node)

ESP32-C6FH4 (QFN32) rev 0.1, **4MB flash in package**, no PSRAM. On USB it shows up as
`/dev/cu.usbmodem1101` like the others. Pins (USB-C up, parts facing you): left column D0-D6, right
column 5V, GND, 3V3, D10-D7. D2 = GPIO2 (no strapping or console role) carries the link. D6/D7 are
UART0 (console), GPIO8/9/15 strapping, GPIO9 BOOT, GPIO15 user LED (active low, unused),
GPIO3/GPIO14 RF switch (left alone: onboard ceramic antenna).
- App 1.69MB in 1.9MB OTA slots (14% free). Internal RAM free ~221KB idle, min 215KB while pairing.
- Paired as "Pill Reminder Dash" with the manual code (no screen) on the first try (2026-11-20).

## CrowPanel 7" facts (dashboard panel)

Elecrow DIS08070H, board rev V3.0, ESP32-S3-WROOM-1-N4R8 (**4MB flash, 8MB PSRAM**), CH340-type
USB serial on `/dev/cu.usbserial-110`. **Only 115200 baud is reliable** (921600/460800 garbled
reads); opening the port resets the board. Factory flash: `~/esp/backups/crowpanel7_factory.bin`
(4MB, verified; restore with `write_flash 0x0`).
- Needs the Xtensa compiler (`idf_tools.py install xtensa-esp-elf`, installed). `export.sh` does
  not put it on PATH yet: prepend
  `~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin` (or register the
  esp32s3 target with the IDF tools before the dashboard work). Use `getidf`, not `getmatter`.
- GPIO_D Grove port: **yellow = IO38, red = 3V3, black = GND, white = NC** (measured). Grove
  UART0 is not used: it shares UART0 with the USB serial chip.
- Speaker (from the V3.0 schematic, Eagle file in Elecrow-RD/CrowPanel-7.0-HMI-ESP32-Display-800x480):
  **NS4168** I2S class-D amp, **BCLK IO42, LRCLK IO18, DATA IO17** (all free), CTRL pulled to 3V3
  (always on, no enable pin), powered from **3V3** (the 5V option R50 is not fitted: ~0.5-1 W).
  Output via 10 uF series caps + 1 nF to GND (weak lows: tones of 1-4 kHz carry best) to **J5, JST
  PH 2.0 2-pin**. Speaker bought: 3 W 8 ohm with a PH 2.0 plug (not yet fitted or coded).
- Pin test / link tester: `~/esp/crowpanel_pintest` (outside this repo; `HANDOFF.md` there). Phase 3
  polls the XIAO and logs replies; BOOT (GPIO0) sends TAKE for the next pending dose.

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
- C6 node (2026-11-19): attempt 1 timed out (LVGL starving Matter, fixed). Attempt 2: the node
  couldn't attach to a Thread router for ~25 s, formed its own partition, merged ~17 s later; Home
  fabric then completed, but the Keychain fabric got AddNOC and no CASE (30 s fail-safe expired,
  both fabrics removed, "Pairing failed"), as on the C5 before. Attempt 3 (after the user was asked
  to recheck the iPhone settings above) completed both fabrics in ~12 s. Named "Med Dot 1"; as an
  empty bridge it doesn't appear in a room (Home Settings > Hubs & Bridges).
- Harmless log noise: `chip[DIS] ... Failed to advertise ...: 3` before joining Thread,
  `Failed to process UDP - Duplicated` (two border routers), unknown-cluster read of `0x1349_FC00`
  (Apple-only cluster), `Long dispatch time`, SEC_CERT_DAC_PROVIDER / unknown kconfig symbol warnings,
  legacy RMT driver warnings.

## UI plan (next work)

- **Plain LVGL** (not ESP-Brookesia: experimental, phone-launcher model, unverified with the
  C++17 Matter forces). Reuse Waveshare's look: icon images (112×112 ARGB8888 C arrays, e.g.
  `examples/factory/components/Settings/assets/img_app_settings.c`), dark stylesheet colors/fonts
  (`examples/factory/main/dark/`), LVGL QR widget usage from their Settings app.
- Design: claude.ai Design artifact (boards "Set B (menu)": MenuNext, MenuOverdue, MenuUndo,
  MenuDone, MenuOpen, MenuSettings, MenuDoses, MenuEdit). The firmware has since moved past the
  boards in places (banner, list height, editor colors): the code is the reference. Names are
  location-neutral (the device may move rooms; more units may go elsewhere). Landscape 320×240,
  light theme default (Settings > Dark mode switches every page, see 7c.5). Current home (7f,
  `main/ui_home.c`, RECT layout):
  - Top: centered banner, 16px bold, date deep blue · 24 h time deep green
    ("THU 19 NOV · 0511 hrs"); hamburger menu button top-right (272,4, 40×32) opens a panel:
    Doses, Settings, About (tap outside or X closes it).
  - Left (y 42-198): today's doses as a compact list (`ui_dose_list_row`), time + name, 13px,
    solid dose color always (taken rows only add "✓"; overdue rows get a red border + pulse; no
    outline on the next dose: an earlier ring / tint made colors look wrong). Max 5 rows (28px);
    fewer rows grow (flex_grow) to fill the height. More than 5: up/down arrows on the far left
    page by 4 and the list snaps so the next dose shows; otherwise the arrows hide and the list
    takes their column. Rows are display only, except hold a taken row 1 s to undo (confirm dialog).
  - Right (x 189, w 123, same height as the list): the next dose as a big card in its color:
    time, name (23px, up to 2 lines), "Tap when taken". The card is the only tap target (she found
    the old rows hard to hit). Overdue: red 3px border, pulse, "Late N min" badge. After a take, a
    10 s Undo strip (amber button, countdown bar) shares the card's space. Nothing left: gray "All
    done today" card with tomorrow's first dose.
  - Bottom: node-sync status ("N nodes synced" green, "x of y nodes synced" red, "No nodes" gray;
    `node_sync_status`) and version, centered on the screen.
  - Taps count only when held >= 100ms (`main/ui_tap.c`, `UI_TAP_MIN_MS`). Layout per board:
    Kconfig `MEDREMINDER_UI_LAYOUT` RECT (C5) or ROUND (node, `main/ui_round.c`, see 7g). Boot splash: Averak Labs logo (cyberpunk recolor) for 5 s
    (`SPLASH_MS` in `main/ui.c`), then a fade to home.
  - Earlier home layouts (rotating pills v0.4.0, full-width today list v0.6.0, the old ROUND
    circle mock-up) are in git history.
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
      design: 8, 9 regular; 9, 10, 11, 12, 13, 14, 16, 23, 30 semibold; 12, 16, 48 bold (30/48
      for the node). Inter 4.1 TTFs:
      download the release zip from github.com/rsms/inter (`extras/ttf/`); not kept in the repo. Glyphs: ASCII + "·" + "✓" only; add
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
    - 7c.4 (done, v0.5.0; list/editor reworked in 7f) dose schedule editing + undo. Settings > Doses (`main/ui_doses.c`): scrollable list
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
      - Per-dose color (`dose_colors`, in `main/dose_colors.c` (shared with the dashboard), index
        saved per dose, append only; the picker offers
        `dose_color_order`, see 7f for the current set). Rows are solid (an opacity option was tried and removed; `opacity` stays in the
        struct for the saved layout).
      - Weekly doses: `days` bitmask (Daily or one weekday roller); only shown/alerted on their
        day; same person + same time allowed on different days. Schedule NVS key `sched2`
        (`dose_def_t` with color/opacity/days, 32 bytes); old `sched` is migrated on load.
      - Light/dark theme (`main/ui_theme.c`, NVS `ui`/`dark`, light default): every screen takes
        colors from `UI_T`; the Settings switch rebuilds home in place (`ui_home_restyle`), drops
        cached Doses/About screens and rebuilds Settings, no reboot.
      - 24 h military time everywhere ("0600"; clock "1312 hrs"); editor hour roller 00-23.
      - Scrollbar gutter (`ui_scroll.c`, removed in 7f for arrow buttons; in git history). LVGL
        list padding is not touchable (plain objects aren't clickable).
      - Rows have one font size (per-row auto-fit looked uneven). A label with LONG_DOT needs a
        fixed height or it wraps instead of truncating.
      - Logo: `tools/cyber_logo.py` recolors `averak_labs_neon.png` -> `averak_labs_cyber.png`
        (yellow AVERAK, glowing magenta strokes); About shows it in a dark tile.

  - **7e** (v1.0.0; endpoints come up at boot; Home-side testing
    still to do) Matter accessories
    (`main/matter_dev.cpp`). The device is a **bridge**: aggregator endpoint 1 (replaces the light
    example's placeholder light) with bridged endpoints:
    - "Dose overdue": Contact Sensor, open while any dose is overdue (Home notifications). Boolean
      State is code-driven: `attribute::update` can't set it; get the cluster from
      `data_model::provider::get_instance().registry()` and call `SetStateValue` (also sends the
      StateChange event).
    - One On/Off Plug-in Unit per dose, NodeLabel "<name> <time>" ("Karen 0930", + " TUE" for
      weekly), on = taken today. Bridged Basic Information: Manufacturer/Product from the device's
      own config, SerialNumber "K-0930-TUE" (initial, time, weekday), UniqueID "dose-<uid>". On/off from Home takes/undoes it (same as a tap); doses not due today stay off
      and writes to them are set back.
    - Stable identity: each dose has a `uid` (1-255, the former padding byte of `dose_def_t`, so
      the `sched2` layout is unchanged; assigned on load for older saves, counter in NVS
      `doses`/`uid`, not reused soon). `matter_dev` saves uid -> endpoint id (NVS `matter_dev`/`eps`)
      and resumes endpoints with their ids at boot, so Home rooms/automations survive edits and
      re-sorting. Add/delete a dose adds/removes its endpoint live; rename updates NodeLabel (the
      device's name wins at every boot). If ids can't be resumed (Matter factory reset), new ones.
    - Threads: the UI task publishes snapshots from `doses_tick` (`matter_dev_publish`) and drains
      Home requests (`matter_dev_next_request`; the attribute callback also fires for the module's
      own updates, so writes coming from the `matter_dev` task (`s_task`) are not requests); a
      `matter_dev` task (PSRAM stack) does the
      endpoint/attribute work. esp-matter's stack lock isn't recursive and `enable()`, `destroy()`,
      `attribute::update()` take it themselves: never call them holding it. Never take the display
      lock from the CHIP task (matter_info takes the stack lock under the display lock).
    - RAM: `ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL` (data model in PSRAM),
      `MAX_DYNAMIC_ENDPOINT_COUNT` 20. Cluster servers reserve RAM per dynamic endpoint: Color
      Control is disabled (a stub for `emberAfPluginLevelControlCoupledColorTempChangeCallback` in
      matter_dev.cpp; Level Control can't go, On/Off links against it). Static .bss +1.1KB vs v0.6.0.
    - To test: re-add the accessory in Home if the old light lingers; tiles per dose; take/undo
      both ways; add/rename/delete a dose live; reboot keeps tiles/rooms; overdue notification;
      heap during commissioning with 16 doses. Home may keep old "<name> <period>" names until it
      re-reads NodeLabel.

  - **7f** (done, v1.0.0) UI rework for the primary user (the
    small rows were hard to tap): home as described under Design; plus
    - Screen sleep (`main/ui_sleep.c`): backlight off after Settings > Screen off (1-60 min or
      Never, NVS `ui`/`sleep`, default 5); a full-screen shield on the top layer swallows the
      waking tap; wakes by itself while a dose is due/overdue and never sleeps then.
    - Settings > Overdue after (5-120 min, NVS `doses`/`overdue`, default 30). Both are
      tap-to-cycle buttons.
    - Settings (`main/ui_settings.c`): no Doses/About buttons (home menu now); pairing status in
      the header; setting buttons + Dark mode in one column; vertical brightness (sun) and volume
      (speaker) sliders on the right with live %.
    - Doses list: 30px rows (time, name, weekday for weekly), arrows on the right page by 5; no
      drag scrolling, so a touch is always a tap. Editor: no period (it is derived from the time:
      <11:00 AM, <16:00 MID, <20:00 PM, else BED; still saved for the round layout and
      `same_dose`), name + hour/minute/repeat rollers on the left, a sample row (same 28px/13px as
      home) level with the name, 6×3 color grid under it.
    - Palette: 25 colors (12 originals, 12 added, light pink #f472b6 last). The picker offers 18
      (`DOSE_PICKER_COUNT`); slate, pale sky, mint, lavender, rose, charcoal (least vibrant) and
      brown are not offered but still render for doses that use them.
    - Dev reset button and Kconfig `MEDREMINDER_DEV_TOOLS` removed (and `doses_reset`).
    - Next: shake-to-wake with the QMI8658 IMU (I2C 0x6B).

  - **7g** (in progress, branch `c6-node`) C6 round node (`boards/c6_round`, see the C6 facts).
    - Step 1 (done) bring-up: display, backlight, orientation, colors verified; Matter + LVGL fit.
    - Step 2 (done) node screen (`main/ui_round.c`, 240x240, read across the room): a 14px ring
      around the edge in the next dose's color (red, blinking when overdue; green when done); date
      and "1412 hrs" at the top; the next dose's time (48px) and name (30px) in the middle with
      "in 25 min" / "Due now" / "Late 42 min"; today's doses as dots along the bottom (filled =
      taken, next one larger; more than 9: "3/12 taken"); version. Not paired: Matter QR + manual
      code (no touch, so no Settings). During setup a static "Pairing / Finish on your phone",
      then "Pairing successful" for 10 s (`matter_pairing_active()` / `_completions()` in
      matter_info.cpp, fed by `app_event_cb`: pairing runs from the PASE session until
      CommissioningComplete or fail-safe expiry; the session's end only counts if the fail-safe was
      never armed, since Thread commissioning finishes after it).
    - Step 3 (done) sync (`main/node_sync.c`): UDP port 49231 on the Thread mesh (not via Apple),
      the C5 is the source of truth. The C5 multicasts a BEACON (CRC of its state) to `ff03::4d52`
      every 30 s and on every change; nodes answer HELLO (the CRC they hold); a node with an old CRC
      gets a STATE (`doses_snapshot_t`: schedule, taken bits, date, overdue setting), saves it in
      NVS and acks with HELLO. The same state goes to a node at most every 5 s; a newer one goes
      at once (an Undo within 5 s of a take used to wait for the next beacon). Synced = heard
      within 120 s with the current CRC; nodes are forgotten after 10 min. The node shows a "Not
      synced" badge when the C5 hasn't been heard for 90 s. Tested: take/undo on the C5 reaches the
      node in ~1 s; unplugging the node turns the C5 red within 2 min.
      - lwIP + OpenThread: `inet6_aton()` into a `struct in6_addr` writes an lwIP `ip6_addr_t`
        (4 bytes longer, zone) and zeroed the next field (`ipv6mr_interface`): the multicast join
        failed with errno 125. Use `ip6addr_aton` + `inet6_addr_from_ip6addr`.
    - Node colors (wife: the white screen was too bright): the whole screen is the next dose's
      color (as its pill on the C5), text white/dark by `ui_dose_name_color`; date 16px, clock
      23px; overdue: red ring blinks, "Late N min" in a white badge; done for the day: dark gray
      with a green ring. Dots in the text color.
    - Node screen dims to 25% after the sleep timeout instead of going off (`ASLEEP_PERCENT` in
      `ui_sleep.c`, NODE role); full brightness while a dose is due. Always-on is fine for the
      IPS LCD (no burn-in); dimming mainly saves the backlight LEDs.
    - Shake to take (`main/imu_shake.c`, replaced the double tap at the wife's request; the tap
      version is in git history before 2026-11-20): gravity is a ~0.5 s average of the samples, a
      swing is the rest going over 600 mg (`SHAKE_MG`, re-armed under half, >= 60 ms apart);
      **4 swings within 1.0 s** (`SHAKE_SWINGS`/`SHAKE_WINDOW_MS`) are a shake, then 2 s cooldown.
      Tuned on the real node: shakes put 4-5 swings in 0.4-0.7 s; picking up, carrying, a bump on
      the table make 1-3, spread over ~1 s (5 in 1.5 s missed a gentle shake). Each swing is
      logged (`imu_shake: swing 2 of 4`). The node shows "Taking...", sends TAKE (uid) to the C5
      (retried every 2 s, 5 times, until a new STATE arrives); the C5 takes it like a Home request
      (`doses.c`), ~2 s round trip. No undo on the node (C5 or dashboard). Logging over USB
      while moving the node gets cut off (the host re-enumerates, the node restarts): test
      handling on a charger and watch for "Taking..." instead.
    - TAKE carries flags in the CRC field (uid in bits 0-7): `TAKE_UNDO` 0x100 (dashboard undo),
      `TAKE_SNOOZE` 0x200 (uid 0; see 7h step 9). API: `node_sync_request(uid, NODE_REQ_TAKE /
      _UNDO / _SNOOZE)` on a node, `node_sync_next_request(&uid, &req)` on the C5. An older C5
      would read an undo as a take: flash the C5 first.
    - Battery: the board has a LiPo charger (GPIO23 status, GPIO0 divider), but as a Thread router
      with the radio always on it would last well under a day. Weeks would need a sleepy end
      device (Matter ICD), the screen mostly off (wake on tap / dose due) and polling sync.
    - Later: undo from the node; battery tile in Home.

  - **7h** (in progress, branch `c6-node`) dashboard: CrowPanel 7" ESP32-S3 fed by a XIAO C6 node.
    - Step 1 (done) `boards/xiao_c6_dash`: headless node, pairs, syncs from the C5 ("2 nodes
      synced").
    - Step 2 (done) single-wire link (`main/uart_link.c`, Kconfig `MEDREMINDER_UART_LINK_GPIO`,
      -1 = none): GPIO2 on the XIAO <-> IO38 on the panel, 115200 8N1, both pads open-drain with
      pull-up and UART TX+RX routed onto the one pad (each side hears and drops its own bytes).
      The panel polls every 200 ms, the node only answers. Frame: 0xA5, type, length u16 LE,
      payload, CRC32 LE (`esp_rom_crc32_le` over type+length+payload). POLL (u32 CRC the panel
      holds), TAKE (CRC + u8 uid, sent on via `node_sync_request`), UNTAKE (0x03), SNOOZE (0x04,
      CRC only), REPLY (time UTC (0 =
      not set), flags bit0 = C5 connected, CRC; plus the `doses_snapshot_t` when the panel's CRC
      differs). Tested: 326/326 polls OK; BOOT on the panel took Karen 1330 on the C5, new state
      back on the panel in ~2.2 s.
    - Wiring while developing: **only yellow (IO38 <-> D2) and black (GND)**, each board on its own
      USB. Red (3V3) only when the XIAO runs from the panel alone (two regulators would otherwise
      share the rail, and an unpowered panel would load the XIAO's regulator); add 100 uF at the
      XIAO then.
    - Not done (handoff idea): mirror On/Off endpoints on the XIAO. Skipped: the C5 already has a
      tile per dose in Home.
    - Step 3 (done) dashboard bring-up: `dashboard/` (separate IDF project, no Matter; `getidf`,
      then `./build.sh`, `./build.sh -p /dev/cu.usbserial-110 -b 115200 flash`; the script puts the
      Xtensa compiler on PATH). `board.c`: RGB panel (pins/timing from the V3.0 schematic and
      Elecrow's factory code, 15 MHz pclk, 3 PSRAM frame buffers + internal bounce buffer; steady,
      no flicker), GT911 (reset via PCA9557 0x18 IO0 with IO1/INT low -> 0x5D; polled, no
      rotation/mirroring needed), backlight LEDC on GPIO2; LVGL via esp_lvgl_adapter. `link.c`:
      the panel side of the link. `doses.h` and `firmware/main/uart_link_proto.h` (frame layout)
      are shared through the include path. PCLK is GPIO0: BOOT is unusable once the display runs.
      - IO38 also reaches the GT911 INT net through R57 ("10K/NC"; schematic note: IO38 is free
        with the resistive touch option). On this unit touching during polling caused 0 link
        failures, so R57 is not fitted (Elecrow's own V3.0 code drives IO38 as plain GPIO_D too).
      - The XIAO can run from the Grove red wire (3V3) with no USB of its own; never both at
        once. Then it restarts with the panel (and with every panel serial-port open).
    - Tiles (no Home Assistant): weather (Open-Meteo) and Cozi (calendar .ics feed) over the S3's
      Wi-Fi, a timer. Dropped: garage/package tiles (replaced by the timer), lock,
      temperature/humidity (HomePod readings aren't reachable outside Home; the Resideo T9/T10
      has a cloud API if wanted later). Possible later: panel buttons as mirror On/Off switches on
      the XIAO (auto-off after ~2 s) that trigger Home automations (e.g. HomePod "Pause Audio").
      Secrets (Wi-Fi, location, Cozi URL) in a gitignored `dashboard/secrets.h` (template
      `dashboard/secrets.h.example`; the user fills it in), never in git or chat. Don't print it or
      log the SSID (the Wi-Fi driver logs it at INFO: `wifi.c` sets the `wifi` tag to WARN).
    - Step 4 (done) dose screen (`dashboard/main/ui.c`, dark): banner date · time (30px), today's
      doses as pills on the left (7 rows max, 23px; more: the window starts one row above the
      next dose), the next dose as a big card in the middle (48px time, 30px name, "Late N min"
      badge, red pulsing border when overdue; tap = TAKE, "Taking..." until the new state or
      10 s), tiles on the right (layout since changed, see step 7). A
      status line only when the node doesn't answer or the C5 isn't connected. States are
      computed from the snapshot as `doses.c` does (taken bits only for the snapshot's date).
      The S3's clock is set from the node's REPLY time. Fonts: the firmware's Inter fonts;
      palette: `firmware/main/dose_colors.c`. User checked: layout, size/readability, colors,
      no flicker.
    - Step 5 (done) undo on the dashboard: hold a taken row 1 s (LVGL long-press time set to
      1000 ms) -> dialog "Undo Karen 2100?" Keep / Undo -> UNTAKE (0x03) over the link -> the
      XIAO sends TAKE with `TAKE_UNDO` -> the C5 undoes it like a Home "off". ~2 s round trip.
    - Step 6 (done) boot splash and schedule view: the Averak Labs cyber logo (263x380 RGB565,
      `dashboard/main/images/`, from `firmware/main/images/src/averak_labs_cyber.png` with
      `firmware/tools/png_image.py --height 380`) for 5 s, then a fade to the dose screen. A
      "Schedule" button (top right) opens every dose in two columns of 8 (time, name, "Daily" or
      weekdays) and the overdue setting, view only; Back, or 60 s untouched, returns.
    - Step 7 (done) Wi-Fi, weather, three-column layout, timer:
      - Layout: three equal columns (`COL_W`). Left: the dose list (5 rows, 16px) over the
        next-dose card (30px time, 23px name), the same height. Middle: calendar (deep teal
        #134e4a, step 8). Right: weather (dark sky blue #075985) level with the list, timer
        (deep purple #4c1d95) level with the card. Colors chosen by the user; no red/orange
        (overdue) on tiles.
      - `wifi.c`: station from `secrets.h` (credentials kept in RAM only), reconnect with backoff;
        without `secrets.h` it builds with Wi-Fi off. Wi-Fi/lwIP and mbedTLS allocate in PSRAM.
        Custom `partitions.csv`: one 3 MB factory app (Wi-Fi + TLS made it 1.4 MB).
      - `weather.c`: Open-Meteo over HTTPS (cert bundle) every 15 min (60 s retry, 2 s while Wi-Fi
        is down): temp (F), humidity, WMO code, today's high/low, max rain chance. Tile: icon +
        "61 F" (the fonts have no degree sign), condition, "H 69 · L 52 · Rain 3%", "as of HHMM"
        when over an hour old. Icons: own 24x24 stroke SVGs in `dashboard/main/icons/svg`
        rendered at 48px with `firmware/tools/svg_icons.py` (sun, partly, cloud, fog, rain, snow,
        storm; recolored per condition).
      - Timer tile: roller 5-60 min in 5 min steps (pinned left), time left centered, Start /
        Pause / Reset. When it runs out, a full-screen "Timer done" (pulsing red border) over any
        screen; a tap closes it and resets. Runs on lv_tick, 200 ms refresh.
    - Step 8 (done) Cozi calendar (`dashboard/main/calendar.c`): the household's Cozi iCalendar
      feed (`DASH_COZI_URL`, rest.cozi.com; **needs a User-Agent**, 403 without) fetched whole
      into PSRAM (~120 KB, 449 events) every 15 min and at midnight, expanded into the next 14
      days (max 64 occurrences). Feed facts (checked 2026-11, structure only, titles never
      printed): timed events `TZID=America/Chicago` (= the panel TZ, taken as local), all-day as
      dates (DTEND exclusive); RRULE only FREQ YEARLY/WEEKLY/MONTHLY with INTERVAL, UNTIL (UTC),
      BYDAY (weekdays or "2SU"/"-1FR"), BYMONTHDAY, BYMONTH; EXDATE one per line; no COUNT or
      RECURRENCE-ID; titles escaped and partly non-ASCII (folded to ASCII for the fonts). The
      expansion was checked on the Mac against python-dateutil for three windows (incl. a DST
      change): identical. **Every title starts with "Evan:", "Karen:" or "All:"**: stripped and
      shown as an initial badge (E red #dc2626, K royal blue #1f45e6 = dose palette 0, A purple
      #9333ea; gray for any other prefix). Tile: a band per day (TODAY brighter, then TOMORROW,
      then "MON 23 NOV"), events as chips (filled = timed with HHMM, outlined = all day or a
      timed event continuing from an earlier day), as many as fit (~10 rows); today's timed
      events drop off when over. To inspect the feed, print counts/shapes only (it's private).
    - Step 9 (done, tested) overdue snooze, all units (`doses_snooze` in `main/doses.c`; the C5
      decides). An overdue dose can be snoozed `DOSE_SNOOZE_MIN` (15) min, at most
      `DOSE_SNOOZE_MAX` (4) times per overdue spell (until nothing is overdue; midnight resets).
      While snoozed: no chime; one snooze at a time; a dose newly overdue ends it. When it ends:
      the chime plays (5 min repeat resumes) and `matter_dev_renotify()` closes the "Dose overdue"
      sensor for 3 s and reopens it, so Home notifies again. `DOSE_TEST`: 2 min snooze.
      - Snapshot: the former `reserved[2]` is `snooze` (`DOSE_SNOOZE_CODE`: bits 0-10 end minute
        of the day + 1, 0 = not snoozed; bits 12-14 snoozes left). An older C5 sends 0: no
        snooze offered.
      - C5 home: an inverted strip under the overdue card (where Undo goes; Undo wins): "N snoozes
        left" + blue "Snooze 15 min" button, or "Quiet until HHMM" while snoozed.
      - Dashboard: the same strip under the next-dose card ("Quiet until HHMM · N left"); tap ->
        SNOOZE over the link -> XIAO -> TAKE_SNOOZE to the C5; "Snoozing..." until the new state
        or 10 s.
      - Round node: can't snooze (no touch); while snoozed, "Late N min" alternates with "Quiet
        until HHMM" every 3 s.
    - The panel logs each dose's time and palette index when a new dose state arrives
      (`link:   1900 color 0`), for matching colors.
    - Next: speaker (audio.c like the C5's: timer chime, overdue chime on the dashboard and
      whether the C5 should keep chiming too, volume). Schedule ownership stays on
      the C5 (moving it would move the Home dose tiles to the XIAO and put the critical job on
      the most complex device); editing from the dashboard (requests the C5 validates) only if
      wanted later.

- Licensing: Waveshare code is Apache-2.0. Keep headers on copied files and list them in a
  `THIRD_PARTY_LICENSES` file. `esp_xiaozhi` has its own license; don't copy it.

## Gotchas

- Root `.gitignore` ignores `*.png`; force-add or adjust before committing PNG icon sources.
- KiCad V1 board has an ESP-12F (ESP8266) next to the XIAO C6: not supported by ESP-IDF and
  conflicts with the no-Wi-Fi device design; open question whether it stays for V2.
