#!/usr/bin/env python3
"""Print hello_luke log lines from the watch as they arrive. Usage: listen.py [seconds, default forever]"""
import serial, time, re, sys
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 1e9
p = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
buf = b''; end = time.time() + secs
while time.time() < end:
    buf += p.read(4096)
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        s = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', line.decode('utf-8', 'replace')).strip()
        if re.search(r'hello_luke|E \(|panic|Guru|rst:', s): print(s, flush=True)
