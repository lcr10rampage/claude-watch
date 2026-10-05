# Watch firmware

For the Waveshare ESP32-S3-Touch-AMOLED-2.06. Coming once the watch arrives.

Plan:
- Wi-Fi + watch face (LVGL)
- Talk button: record audio → transcribe → `POST /ask` → show the answer
- Brief button: `GET /brief`
- Every minute: `GET /reminders/due` → vibrate/beep and show any reminders
- Bluetooth ANCS: show iPhone notifications directly from the phone
- On-watch extras: step counter (IMU), timers, alarms

All server requests send the `X-Watch-Token` header.
