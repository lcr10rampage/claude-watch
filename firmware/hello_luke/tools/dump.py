#!/usr/bin/env python3
"""Dump the watch's on-device event log (last 512 touch/controller/screen events).
Usage: dump.py [/dev/ttyACM0]   (run with the ESP-IDF venv python for pyserial)"""
import serial, sys, time, re
port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM0'
KIND = {'B': 'BOOT (reset reason)', 'T': 'tap at', 'C': 'contact at', 'D': 'double-tap -> screen',
        'S': 'screen', 'E': 'touch I2C error start (code)', 'e': 'touch I2C error end (ms, count)', 'R': 'controller reset'}
REASON = {1: 'power-on', 3: 'software', 4: 'panic', 5: 'int wdt', 6: 'task wdt', 7: 'other wdt', 8: 'deep sleep', 9: 'brownout', 10: 'sdio', 11: 'usb', 12: 'jtag'}
with serial.Serial(port, 115200, timeout=0.3) as p:
    time.sleep(0.2); p.reset_input_buffer()
    p.write(b'DUMP\n'); p.flush()
    buf = b''; end = time.time() + 15
    while time.time() < end:
        buf += p.read(4096)
        if b'END\n' in buf: break
text = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', buf.decode('utf-8', 'replace'))
for line in text.splitlines():
    if line.startswith('EVLOG') or line == 'END':
        print(line); continue
    m = re.match(r'\s*(\d+)\.(\d+) (\S) (-?\d+) (-?\d+)', line)
    if not m: continue
    t = f"{int(m[1])//3600:02d}:{int(m[1])%3600//60:02d}:{int(m[1])%60:02d}.{m[2]}"
    k, a, b = m[3], int(m[4]), int(m[5])
    desc = KIND.get(k, k)
    if k == 'B': detail = REASON.get(a, str(a))
    elif k in ('T', 'C'): detail = f"({a}, {b})"
    elif k == 'D': detail = 'on' if a else 'off'
    elif k == 'S': detail = 'on' if a else 'off'
    elif k == 'e': detail = f"{a} ms, {b} failures"
    elif k == 'E': detail = f"0x{a:x}"
    else: detail = ''
    print(f"{t}  {desc} {detail}")
