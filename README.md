# Paperless recorder — LVGL UI

LVGL v9 UI for the 40-channel paperless recorder (CM4 + 5×8-ch I2C cards).
Runs identically as a PC simulator (SDL window, mouse = touch) and on the
Raspberry Pi (DRM → DSI panel, evdev touch). No desktop/X11 needed on the Pi.

## What's implemented

- Status bar: view title, group prev/next (CH 1–8 … 33–40), batch tag, live clock
- Digital view: 4×2 channel tiles, alarm channels turn red (watch CH3)
- Trend view: scrolling group trend, 8 series, 2 min history, legend
- Bar / Alarm / Menu: placeholders
- `data_model.c`: fake data simulator — on real hardware, the I2C poll
  thread writes into `g_ch[]` instead and **no UI code changes**

## Build on Windows (simulator)

One-time setup — install MSYS2 from https://www.msys2.org, then in the
**MSYS2 UCRT64** terminal:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 git
```

Build and run (first build downloads LVGL automatically, needs internet):

```bash
cd /c/Users/DELL/Downloads/RECORDER_UI_LVGL
cmake -B build -G Ninja
cmake --build build
./build/recorder_ui.exe
```

## Build on Raspberry Pi

```bash
sudo apt install cmake build-essential git libdrm-dev
cd RECORDER_UI_LVGL
cmake -B build -DPI_BUILD=ON
cmake --build build -j4
sudo ./build/recorder_ui        # run from console, not from a desktop
```

Device paths are runtime arguments — no rebuild needed when switching
between HDMI and DSI:

```bash
# HDMI monitor + USB mouse (prototype)
sudo ./build/recorder_ui /dev/dri/card1 /dev/input/event0

# DSI 7" touch panel (later) — find the touch event with `evtest`
sudo ./build/recorder_ui /dev/dri/card0 /dev/input/event4
```

Notes for the Pi:
- `ls /dev/dri` shows the available DRM cards; try the other card if the
  screen stays black.
- `evtest` lists input devices and lets you verify touch/mouse events.
- HDMI needs nothing in config.txt; the official 7" DSI panel is
  auto-detected on Raspberry Pi OS (KMS driver, default config).
- Autostart + silent boot: `sudo bash scripts/setup-kiosk.sh` (hides all
  boot text/logos/login prompt and starts the UI automatically on power-on;
  SSH stays available for development).

## File map

| File | Purpose |
|---|---|
| `lv_conf.h` | LVGL config (fonts, SDL vs DRM driver) |
| `src/main.c` | init, display/input driver, main loop |
| `src/data_model.{c,h}` | 40-channel latest-values table + simulator |
| `src/ui/ui.{c,h}` | colors, root layout, nav bar, view switching |
| `src/ui/scr_digital.c` | home screen tiles |
| `src/ui/scr_trend.c` | group trend chart |

## Next steps (in order)

1. Bar view (`lv_bar` per channel, same tile grid)
2. Alarm view (`lv_table` of active/historic alarms + acknowledge)
3. Menu → channel config forms (`lv_dropdown`, keyboard widget)
4. SQLite logging thread + trend history from DB (scroll back in time)
5. Replace simulator with I2C poll thread (`/dev/i2c-1`, burst read + CRC)
