#!/usr/bin/env python3
"""Read the watch's persistent battery log and report drain per state.
Usage: battlog.py [--clear]      (uses /dev/ttyACM0; needs pyserial: run with /usr/bin/python3)"""
import serial, sys, time, re
from datetime import datetime

p = serial.Serial('/dev/ttyACM0', 115200, timeout=0.05); time.sleep(0.3); p.reset_input_buffer()
def cmd(line, end, wait=20):
    p.write((line + "\n").encode()); p.flush(); buf = ""; t = time.time() + wait
    while time.time() < t:
        c = p.read(p.in_waiting or 1).decode("utf-8", "replace")
        if c:
            buf += c
            if end in buf: break
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf)

if "--clear" in sys.argv:
    print(cmd("BATTLOG CLEAR", "battlog").strip().splitlines()[-1]); sys.exit()

rows = []
for l in cmd("BATTLOG", "END battlog").splitlines():
    m = re.match(r"B (\d+) (\d+) (-?\d+) ([CDS]) ([01]) ([01])$", l.strip())
    if m:
        t, mv, pct, st, scr, wifi = m.groups()
        rows.append((int(t), int(mv), int(pct), st, int(scr), int(wifi)))
if not rows:
    print("battery log is empty"); sys.exit()

print(f"{len(rows)} entries, {datetime.fromtimestamp(rows[0][0]):%b %d %H:%M} -> {datetime.fromtimestamp(rows[-1][0]):%b %d %H:%M}")
print(f"{'time':<13}{'mV':>6}{'%':>5}  state screen wifi")
for t, mv, pct, st, scr, wifi in rows:
    print(f"{datetime.fromtimestamp(t):%b %d %H:%M}{mv:>6}{pct:>5}  {st:^5} {'on ' if scr else 'off':^6} {'on' if wifi else 'off':^4}")

# drain per segment while discharging, grouped by screen/wifi state of the segment start
groups = {}
for a, b in zip(rows, rows[1:]):
    dt_h = (b[0] - a[0]) / 3600
    if a[3] != 'D' or b[3] != 'D' or dt_h <= 0 or dt_h > 2: continue
    key = f"screen {'on' if a[4] else 'off'}, wifi {'on' if a[5] else 'off'}"
    g = groups.setdefault(key, [0.0, 0.0]); g[0] += dt_h; g[1] += (a[1] - b[1])
print("\nDischarge rate by state (mV per hour, higher = faster drain):")
for k, (h, dmv) in sorted(groups.items(), key=lambda kv: -kv[1][0]):
    print(f"  {k:<26} {h:5.1f} h   {dmv / h if h else 0:7.1f} mV/h")
