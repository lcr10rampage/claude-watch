#!/usr/bin/env python3
"""Push calendar weeks to the watch over USB serial.

  push_calendar.py --weeks          print the week ranges to fetch (this month + next), as JSON
  push_calendar.py [events.json]    convert data/calendar/events.json to watch lines and upload

events.json format (written by the /calendar-refresh skill):
  {"weeks": [{"start": "2026-10-18",
              "events": [{"summary": "...", "start": "2026-10-19T15:30:00-04:00",
                          "end": "2026-10-19T18:00:00-04:00", "allDay": false, "location": ""}]}]}
All-day events use dates: "start": "2026-10-21", "end": "2026-10-22" (end exclusive), "allDay": true.
"""
import json, sys, time, re, calendar
from datetime import date, datetime, timedelta
from pathlib import Path

PORT = "/dev/ttyACM0"
DATA = Path.home() / "claude-watch" / "data" / "calendar" / "events.json"
MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]


def weeks_for_month(y, m):
    first = date(y, m, 1)
    start = first - timedelta(days=(first.weekday() + 1) % 7)     # Sunday on/before the 1st
    last = date(y, m, calendar.monthrange(y, m)[1])
    out = []
    while start <= last:
        out.append(start)
        start += timedelta(days=7)
    return out


def week_ranges():
    today = date.today()
    y, m = today.year, today.month
    ny, nm = (y + 1, 1) if m == 12 else (y, m + 1)
    starts = sorted(set(weeks_for_month(y, m) + weeks_for_month(ny, nm)))
    return [{"start": s.isoformat(), "end_exclusive": (s + timedelta(days=7)).isoformat()} for s in starts]


def clean(text, limit=110):
    text = (text or "").replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = text.replace("–", "-").replace("—", "-").replace("\n", " ")
    text = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in text).strip()
    return text[:limit - 1] + "…" if len(text) > limit else text


def fmt_time(dt):
    h = dt.hour % 12 or 12
    return f"{h}:{dt.minute:02d} {'AM' if dt.hour < 12 else 'PM'}"


def parse(s):
    if len(s) == 10:
        return datetime.fromisoformat(s), True
    return datetime.fromisoformat(s).replace(tzinfo=None), False


def week_lines(week):
    start = date.fromisoformat(week["start"])
    evs = []
    for e in week.get("events", []):
        try:
            s, s_all = parse(e["start"]); en, _ = parse(e["end"])
        except (KeyError, ValueError):
            continue
        evs.append((s, en, e.get("allDay", s_all), clean(e.get("summary") or "(no title)"), clean(e.get("location") or "", 40)))
    lines = []
    for i in range(7):
        d = start + timedelta(days=i)
        lines.append(f"D {d.isoformat()} {DAYS[d.weekday()]}, {MONTHS[d.month - 1]} {d.day}")
        todays = []
        for s, en, allday, title, loc in evs:
            if allday:
                if s.date() <= d < en.date():
                    todays.append((0, "All day", title, loc))
            else:
                if s.date() <= d <= en.date() and not (en.date() == d and en.time() == datetime.min.time() and s.date() != d):
                    if s.date() == d: t = fmt_time(s)                 # start time only: short rows
                    elif en.date() == d: t = f"until {fmt_time(en)}"
                    else: t = "All day"
                    todays.append((s.hour * 60 + s.minute if s.date() == d else -1, t, title, loc))
        for _, t, title, loc in sorted(todays, key=lambda x: x[0]):
            lines.append(f"E {t}|{title}")
    return lines


class Watch:
    def __init__(self):
        import serial
        self.p = serial.Serial(PORT, 115200, timeout=0.05)
        time.sleep(0.2); self.p.reset_input_buffer()

    def cmd(self, line, expect="OK", wait=2.0):
        self.p.write((line + "\n").encode("utf-8")); self.p.flush()
        end = time.time() + wait; buf = ""
        while time.time() < end:
            chunk = self.p.read(self.p.in_waiting or 1).decode("utf-8", "replace")
            if chunk:
                buf += chunk
                if expect in buf or "ERR" in buf: break
        buf = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf)
        if expect not in buf:
            raise RuntimeError(f"watch did not ack {line[:40]!r}: {buf.strip()[-120:]}")
        return buf


def main():
    if "--weeks" in sys.argv:
        print(json.dumps(week_ranges(), indent=1)); return
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA
    data = json.loads(path.read_text())
    w = Watch()
    w.cmd("CALCLEAR", "OK calclear", 6)
    for week in data["weeks"]:
        lines = week_lines(week)
        w.cmd(f"CALBEGIN {week['start']}", "OK calbegin")
        for ln in lines:
            w.cmd(f"CALADD {ln}", "OK")
        w.cmd("CALEND", "calend", 4)
        n_ev = sum(1 for l in lines if l.startswith("E "))
        print(f"week {week['start']}: {n_ev} events, {len(lines)} lines")
    print(w.cmd("CALLIST", "slots used", 3).strip())


if __name__ == "__main__":
    main()
