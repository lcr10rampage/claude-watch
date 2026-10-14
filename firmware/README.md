# Watch firmware

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06.

## `hello_luke/` — current watch face (ESP-IDF v5.5, LVGL 9.5)

What it does today:

- **Photo face** (boot default): a random one of the pictures from
  `attachments.zip`, with the time (no seconds) tucked at the top.
- **Tap the time** → black **clock face**: "Hello Luke", time with seconds,
  AM/PM, date. Tap the time again to go back.
- **Drag right-to-left** on the photo face → **stopwatch** (full-screen black
  sheet, Start/Stop + Reset). Drag left-to-right to close it.
- **Double-tap the top-right corner** (right 40 % × top 30 % of the glass, taps
  ≤ 500 ms apart) → screen off; same again → screen on. Works on every face
  and while the screen is dark. Waking always lands on the photo face with a
  fresh picture.
- Time comes from the on-board PCF85063 RTC, seeded from the build timestamp
  on first boot and resyncable from the Pi with `./settime.sh`.

Slap / palm-cover detection was tried for two days and dropped: on the wrist a
palm shows up as a normal contact, as a controller lockout, or as nothing at
all, and slap jolts overlap hard taps. The accelerometer task still runs for
logging only.

### Build & flash (on the Pi)

```bash
# one-time: ESP-IDF v5.5.x installed in ~/esp/esp-idf, activated via a wrapper
# that strips any Python venv from PATH (ESP-IDF refuses to run inside one):
. ~/esp/idf-env.sh

cd firmware/hello_luke

# pictures are not committed (4 MB of raw RGB565); regenerate them from the
# photos in attachments.zip (unzip it somewhere first), excluding any you like:
/usr/bin/python3 -I tools/make_pics.py /path/to/unzipped main/pics 1000002544 1000017046

idf.py set-target esp32s3        # first time only
idf.py -p /dev/ttyACM0 -b 921600 flash
./settime.sh                     # push the Pi's current time to the watch
```

Stop any serial listener before flashing; the port cannot be shared.

### Serial console (115200 on /dev/ttyACM0)

`SETTIME YYYY-MM-DD HH:MM:SS`, `GETTIME`, `TIMETAP` (photo ↔ clock face),
`SWIPE LEFT|RIGHT` (stopwatch in/out), `DTAP` (toggle screen), `TAP`,
`LOG ON|OFF` (verbose touch/accelerometer logging).

### Helper scripts (`tools/`, run with the ESP-IDF venv's python for pyserial)

- `cmd.py "SWIPE UP"` — send one console command and show the reply/log.
- `reset_and_log.py [secs]` — hard-reset the watch and print its boot log.
- `listen.py` — stream the watch's log lines.
- `make_pics.py` — photos → 410×502 RGB565 `.bin` files for the Home face.

### Hardware notes learned the hard way

- FT3168 touch: reports a single point only, weight/area always 0. A palm puts
  it into monitor mode: it stops acknowledging I2C for ≥4 s (longer if the hand
  stays) and emits an all-0xFF frame on the way back (dropped). Register 0x86=1
  enables auto-monitor, 0x87=0x0a is the idle timer in seconds; a hardware reset
  (GPIO 9, safe for the display) restores those defaults.
- QMI8658 driver (`waveshare/qmi8658`) returns milli-g; the first sample after
  boot is junk.
- Flash chip is 32 MB but the build is configured for 16 MB (fine for now).

## Still planned (see `WATCH_DESIGN.md`)

Wi-Fi, Talk/Brief buttons against `server/`, reminders polling, ANCS.
