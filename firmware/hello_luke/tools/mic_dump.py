#!/usr/bin/env python3
"""Pull the watch's last recording over USB and save it as a WAV.
Usage: mic_dump.py out.wav           (run with /usr/bin/python3, needs pyserial)
       mic_dump.py --rec 5 out.wav   record 5 s first (MICREC), then dump"""
import serial, sys, time, re, base64, wave

args = sys.argv[1:]
rec = 0
if args and args[0] == "--rec": rec = int(args[1]); args = args[2:]
out = args[0] if args else "mic.wav"
p = serial.Serial('/dev/ttyACM0', 115200, timeout=0.05); time.sleep(0.3); p.reset_input_buffer()
def cmd(line, end, wait):
    p.write((line + "\n").encode()); p.flush(); buf = ""; t = time.time() + wait
    while time.time() < t:
        c = p.read(p.in_waiting or 1).decode("utf-8", "replace")
        if c:
            buf += c
            if end in buf: break
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf)
if rec:
    print(cmd(f"MICREC {rec}", "OK", 3).strip().splitlines()[-1]); print(f"recording {rec} s... speak now"); time.sleep(rec + 0.8)
txt = cmd("MICDUMP", "MICEND", 60)
data = b"".join(base64.b64decode(l[7:]) for l in txt.splitlines() if l.startswith("MICB64 "))
m = re.search(r"MICEND (\d+) (\d+)", txt)
rate = int(m.group(2)) if m else 16000
with wave.open(out, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(data)
import struct
n = len(data) // 2
peak = max((abs(x) for x in struct.unpack(f"<{n}h", data)), default=0)
print(f"saved {out}: {n/rate:.2f} s, {n} samples, peak {peak} / 32767 ({20*__import__('math').log10(peak/32768+1e-9):.0f} dBFS)")
