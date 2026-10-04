# Watch firmware

Coming once the watch model is chosen (LilyGo T-Watch S3 or Waveshare ESP32-S3).
It will: connect to Wi-Fi, show a watch face (LVGL), and on button press send
`POST /ask` with header `X-Watch-Token` to the server, then display the answer.
