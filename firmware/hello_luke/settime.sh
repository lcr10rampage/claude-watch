#!/bin/bash
# Push the Pi's current local time to the Hello Luke watch firmware over USB.
# Usage: ./settime.sh [/dev/ttyACM0]
PORT=${1:-/dev/ttyACM0}
PY="$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python"
exec "$PY" - "$PORT" <<'PYEOF'
import serial, sys, time
port = sys.argv[1]
with serial.Serial(port, 115200, timeout=0.2) as p:
    time.sleep(0.2)
    p.reset_input_buffer()
    p.write(b"SETTIME " + time.strftime("%Y-%m-%d %H:%M:%S").encode() + b"\n")
    p.flush()
    reply = b""
    end = time.time() + 3
    while time.time() < end and b"\n" not in reply:
        reply += p.read(256)
    line = next((l for l in reply.decode("utf-8", "replace").splitlines() if "OK " in l or "ERR" in l), "")
    if "OK " in line:
        line = line[line.index("OK "):]
        print("Watch clock set:", line)
    else:
        print("No reply from watch:", repr(reply))
        sys.exit(1)
PYEOF
