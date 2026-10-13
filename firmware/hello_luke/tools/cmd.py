#!/usr/bin/env python3
"""Send one console command to the watch and print the reply + any log lines for a moment. Usage: cmd.py "TAP" [seconds]"""
import serial, sys, time, re
cmd = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
p = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
time.sleep(0.2); p.reset_input_buffer()
p.write(cmd.encode() + b"\n"); p.flush()
buf = b''; end = time.time() + secs
while time.time() < end: buf += p.read(4096)
p.close()
s = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', buf.decode('utf-8', 'replace'))
for l in s.splitlines():
    if l.strip() and not l.startswith('I (') or 'hello_luke' in l or 'E (' in l: print(l[:160])
