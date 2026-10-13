# Watch firmware

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06.

## `hello_luke/` — current watch face (ESP-IDF v5.5, LVGL 9.5)

What it does today:

- **Clock face** (boot default): "Hello Luke", time with seconds, AM/PM, date.
  Time comes from the on-board PCF85063 RTC, seeded from the build timestamp on
  first boot and resyncable from the Pi with `./settime.sh`.
- **Swipe up** → **Home face**: time tucked at the top over a random photo
  (one of the pictures from `attachments.zip`, a different one every time).
- **Drag right-to-left on Home** → **Stopwatch**: full-screen black sheet with
  Start/Stop and Reset. Drag left-to-right to close. Swipe down on Home → Clock.
- **Palm slap** anywhere → screen off and a silent reset to the Clock face;
  slap again → screen on. Detection = any touch contact + a ≥2.5 g jolt from
  the QMI8658 accelerometer within 300 ms (fingertip taps stay under ~1 g,
  a table bump has no touch).
- Tap is reserved for UI actions (stopwatch buttons today).

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

`SETTIME YYYY-MM-DD HH:MM:SS`, `GETTIME`, `SWIPE UP|DOWN|LEFT|RIGHT`, `SLAP`,
`TAP`, `LOG ON|OFF` (verbose touch/accelerometer calibration logging).

### Helper scripts (`tools/`, run with the ESP-IDF venv's python for pyserial)

- `cmd.py "SWIPE UP"` — send one console command and show the reply/log.
- `reset_and_log.py [secs]` — hard-reset the watch and print its boot log.
- `listen.py` — stream the watch's log lines.
- `make_pics.py` — photos → 410×502 RGB565 `.bin` files for the Home face.

### Hardware notes learned the hard way

- FT3168 touch: reports a single point only, weight/area always 0. A held palm
  makes it stop acknowledging on I2C for the whole hold, then it emits an
  all-0xFF frame (dropped by the firmware).
- QMI8658 driver (`waveshare/qmi8658`) returns milli-g; the first sample after
  boot is junk.
- Flash chip is 32 MB but the build is configured for 16 MB (fine for now).

## Still planned (see `WATCH_DESIGN.md`)

Wi-Fi, Talk/Brief buttons against `server/`, reminders polling, ANCS.
