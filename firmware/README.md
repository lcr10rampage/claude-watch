# Watch firmware

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06.

## `hello_luke/` — current watch face (ESP-IDF v5.5, LVGL 9.5)

What it does today:

- **Photo face** (boot default): a random one of the pictures from
  `attachments.zip`, with the time (no seconds) tucked at the top. The picture
  changes only when the screen wakes from off, not when you visit other faces.
- **Tap the time** → black **clock face**: "Hello Luke", time with seconds,
  AM/PM, date, and a live battery line (percent + volts, refreshed every 3 s;
  green while charging, red at 15 % or below). Tap the time again to go back.
- **Drag right-to-left** on the photo face → **stopwatch** (full-screen black
  sheet, Start/Stop + Reset). Drag left-to-right to close it.
- **Swipe down** on the photo face → **calendar**: a panel slides in listing
  this month's Sunday-to-Saturday weeks (current week blue). Tap a week for a
  scrollable day-by-day list: big day headers (today blue), a rule between
  days, one row per event with the start time in a grey column. The `<` button
  goes back; swipe up on the week list closes the panel. Weeks live in flash
  (`main/calstore.c`: twelve 16 KB raw slots at the start of the otherwise
  unused `storage` partition) and are loaded over USB by the Pi's
  `/calendar-refresh` skill, see `../README.md`. Nothing is fetched by the watch.
- **Double-tap the bottom-right corner** (right 40 % × bottom 30 % of the
  glass, taps ≤ 500 ms apart) → screen off; same again → screen on. Works on
  every face and while the screen is dark. Waking always lands on the photo
  face with a fresh picture.
- **Claude (BOOT button)**: press BOOT → the photo dims, an orange ring hugs the
  glass, and buttons appear: Date, Weather, Latest email, Calendar today. Tap
  one → the ring pulses while the Pi asks Claude → the answer appears in a box
  on an orange screen. Tap the answer or press BOOT to close; BOOT also closes
  the menu. (Voice input is step 3; `ASK <text>` over serial sends any question.)
- **Wi-Fi**: joins at boot and reconnects on its own. `main/secrets.h`
  (git-ignored; copy `secrets.h.example`) holds a list of known networks
  (home Wi-Fi first, then e.g. a phone hotspot); after two failed attempts on
  one the watch moves to the next. Wi-Fi power save is off for link reliability.
- **Works anywhere**: the server address is the Pi's public Tailscale Funnel
  URL (`https://<pi>.<tailnet>.ts.net`, see `../README.md`), so the watch
  reaches Claude from any Wi-Fi or a phone hotspot. HTTPS uses the built-in
  certificate bundle; mbedTLS buffers live in PSRAM (`sdkconfig.defaults`).
- **Sleep mode**: when the screen is off the CPU drops to 80 MHz and light-sleeps
  between events, LVGL timers pause, touch polls slower, the IMU task is
  suspended. Light sleep is automatically disabled while USB is connected.
- **Battery**: read from the AXP2101 (`BATT` over serial), logged to the event
  ring every 5 minutes.
- Time comes from the on-board PCF85063 RTC, seeded from the build timestamp
  on first boot and resyncable from the Pi with `./settime.sh`.

Slap / palm-cover detection was tried for two days and dropped: on the wrist a
palm shows up as a normal contact, as a controller lockout, or as nothing at
all, and slap jolts overlap hard taps. The accelerometer task still runs for
logging only.

### Memory layout (why `components/esp32_s3_touch_amoled_2_06` exists)

Wi-Fi and the display both want the ESP32-S3's ~260 KB of internal RAM. The
stock BSP let LVGL's draw buffer land in PSRAM and the SPI driver then needed a
temporary DMA copy per flush, which fails once Wi-Fi fragments internal RAM.
The local copy of the BSP (takes precedence over the managed one) sets
`buff_dma = true`, `double_buffer = true`, `sw_rotate = false` and a 50-row
buffer: two 41 KB DMA buffers allocated once at boot, after Wi-Fi is started
(`net_init()` runs before `bsp_display_start()`). Single-buffered direct DMA
produced lines and colour distortion; double buffering fixed it. Plus in
`sdkconfig.defaults`: Wi-Fi/LVGL code kept out of IRAM, task stacks in PSRAM
(`xTaskCreateWithCaps`), `SPIRAM_MALLOC_ALWAYSINTERNAL=2048`. Result: ~100 KB
internal free with everything running (`HEAP` over serial).

### Network gotchas

- The watch is 2.4 GHz only. An iPhone hotspot needs **Maximize Compatibility**
  on, and iOS stops advertising the hotspot ~90 s after you leave the Personal
  Hotspot screen if nothing is connected: open that screen, let the watch join,
  then it stays alive. iOS broadcasts a curly apostrophe in "Luke’s iPhone";
  the SSID in `secrets.h` must match byte for byte (`SCAN` shows what the watch sees).
- Since traffic goes through the Funnel, the Pi's band no longer matters. If you
  ever go back to a LAN address: the old router bridged its 2.4 GHz and 5 GHz
  radios badly and the Pi had to be locked to 2.4 GHz
  (`nmcli connection modify <profile> 802-11-wireless.band bg`).

### Build & flash (on the Pi)

```bash
# one-time: ESP-IDF v5.5.x installed in ~/esp/esp-idf, activated via a wrapper
# that strips any Python venv from PATH (ESP-IDF refuses to run inside one):
. ~/esp/idf-env.sh

cd firmware/hello_luke

# pictures are not committed (4 MB of raw RGB565); regenerate them from the
# photos in attachments.zip (unzip it somewhere first), excluding any you like:
/usr/bin/python3 -I tools/make_pics.py /path/to/unzipped main/pics 1000002544 1000017046

cp main/secrets.h.example main/secrets.h   # then fill in Wi-Fi + server token
idf.py set-target esp32s3        # first time only
idf.py -p /dev/ttyACM0 -b 921600 flash
./settime.sh                     # push the Pi's current time to the watch
```

Stop any serial listener before flashing; the port cannot be shared.

### Serial console (115200 on /dev/ttyACM0)

`SETTIME YYYY-MM-DD HH:MM:SS`, `GETTIME`, `TIMETAP` (photo ↔ clock face),
`SWIPE LEFT|RIGHT` (stopwatch in/out), `DTAP` (toggle screen), `BOOT` (Claude
menu), `ASK <question>`, `WIFI` (ip, rssi, server reachable), `HEAP`, `BATT`,
`SCAN` (list visible networks), `NEXTNET` (drop to the next known network),
`SWIPE DOWN|UP` (calendar in/out), `CALOPEN n` (open week n), calendar upload
`CALCLEAR`, `CALBEGIN YYYY-MM-DD`, `CALADD <line>`, `CALEND`, `CALLIST`,
`DUMP` (on-device event log), `LOG ON|OFF` (verbose touch/accelerometer logging).

### Helper scripts (`tools/`, run with the ESP-IDF venv's python for pyserial)

- `cmd.py "DTAP"` — send one console command and show the reply/log.
- `dump.py` — print the watch's event log: last 512 taps, contacts, touch I2C
  errors, controller resets and screen on/off, kept in noinit RAM so it
  survives a software reset. Run it after a bad day to see what happened.
- `reset_and_log.py [secs]` — hard-reset the watch and print its boot log.
- `listen.py` — stream the watch's log lines.
- `make_pics.py` — photos → 410×502 RGB565 `.bin` files for the Home face.

### Hardware notes learned the hard way

- FT3168 touch: reports a single point only, weight/area always 0. By default
  it drops into monitor mode after ~10 s idle (and on a palm), where it stops
  acknowledging I2C and swallows the first tap. The firmware writes 0x86=0 /
  0x87=0xFF at boot and after any reset to keep it awake. A 10 s touch
  watchdog pulses its reset line (GPIO 9, safe for the display) if it goes
  silent anyway, resetting the I2C bus first if the failure was a timeout.
- QMI8658 driver (`waveshare/qmi8658`) returns milli-g; the first sample after
  boot is junk.
- Flash chip is 32 MB but the build is configured for 16 MB (fine for now).

## Still planned (see `WATCH_DESIGN.md`)

Wi-Fi, Talk/Brief buttons against `server/`, reminders polling, ANCS.
