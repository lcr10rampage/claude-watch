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
- **Double-tap the bottom-right corner** (right 40 % × bottom 30 % of the
  glass, taps ≤ 500 ms apart) → screen off; same again → screen on. Works on
  every face and while the screen is dark. Waking always lands on the photo
  face with a fresh picture.
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
