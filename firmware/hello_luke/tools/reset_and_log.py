#!/usr/bin/env python3
"""Hard-reset the watch over USB and print its boot log (filtered). Usage: reset_and_log.py [seconds]"""
import serial, time, re, sys
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
p = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
p.dtr = False; p.rts = False; time.sleep(0.1); p.rts = True; time.sleep(0.1); p.rts = False
buf = b''; end = time.time() + secs
while time.time() < end: buf += p.read(4096)
p.close()
s = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', buf.decode('utf-8', 'replace'))
pat = re.compile(r'hello_luke|qmi|QMI|E \(|W \(|panic|abort|Guru|cpu_start: cpu freq|rst:', re.I)
for l in s.splitlines():
    if l.strip() and pat.search(l): print(l[:170])
