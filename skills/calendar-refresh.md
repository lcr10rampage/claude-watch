---
name: calendar-refresh
description: Refresh the Google Calendar weeks stored on Luke's watch. Use when the user says to refresh, update, reload or sync the watch calendar, or runs /calendar-refresh. The watch must be plugged into the Pi over USB.
---

# Refresh the watch calendar

The watch keeps one text block per week (Sunday to Saturday) in flash. This skill fetches the
real events from Google Calendar for every week of this month and next month, converts them, and
uploads them over USB serial. Stale weeks on the watch are wiped first, so the watch always ends up
with exactly the current set.

## Steps

1. **Check the watch is plugged in**: `ls /dev/ttyACM0`. If missing, stop and tell the user to
   plug the watch in. Stop any serial listener (`pkill -f listen.py`) before uploading.

2. **Get the week ranges**: run
   `/usr/bin/python3 ~/claude-watch/tools/push_calendar.py --weeks`
   It prints a JSON list of `{start, end_exclusive}` dates (about 9 to 10 weeks).

3. **Fetch the events** with the Google Calendar connector, one call per week:
   `mcp__claude_ai_Google_Calendar__list_events` with
   `startTime = "<start>T00:00:00"`, `endTime = "<end_exclusive>T00:00:00"`,
   `orderBy = "startTime"`, `pageSize = 250`. Follow `nextPageToken` if present.
   These calls are read-only. Do not create or change any event.

4. **Write the compact file** `~/claude-watch/data/calendar/events.json`:
   ```json
   {"weeks": [
     {"start": "2026-10-18",
      "events": [
        {"summary": "Marching Band Practice", "start": "2026-10-19T15:30:00-04:00",
         "end": "2026-10-19T18:00:00-04:00", "allDay": false, "location": "Christian Academy of Louisville"},
        {"summary": "Concert", "start": "2026-10-21", "end": "2026-10-22", "allDay": true}
      ]}
   ]}
   ```
   Rules: one entry per week from step 2, in order, even if it has no events (`"events": []`).
   Use `start.dateTime` / `end.dateTime` as-is for timed events. For all-day events the API gives
   `start.date` / `end.date` like `2026-10-21T00:00:00Z`: write just the date part, `allDay: true`,
   and remember `end` is exclusive. Skip events with `status` `cancelled`. Copy `summary` exactly;
   `location` is optional. Write the file with the Write tool in one go.

5. **Upload**: `/usr/bin/python3 ~/claude-watch/tools/push_calendar.py`
   It clears the watch's calendar slots, sends every week, and prints one line per week plus the
   slot list. It must be run with `/usr/bin/python3` (that one has pyserial).

6. **Report** in two or three lines: how many weeks and events went up, and anything skipped.
   On the watch: swipe down from the photo face, tap a week, scroll through the days.
