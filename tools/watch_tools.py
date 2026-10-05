"""
Your custom tools for the watch.

Every function marked @mcp.tool() becomes something Claude can use.
To connect a new API:
  1. Write a function below that calls the API and returns text.
  2. Give it a clear docstring (Claude reads it to decide when to use it).
  3. Add a line about it to brain/CLAUDE.md.
Claude starts this file fresh on every question, so there's nothing to restart.

API keys and settings go in tools/.env (never in this file).
"""

import json
import os
import sys
import urllib.parse
import urllib.request
from datetime import timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

# Load settings from tools/.env
_env = Path(__file__).with_name(".env")
if _env.exists():
    for line in _env.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, _, value = line.partition("=")
            os.environ.setdefault(key.strip(), value.strip())

from mcp.server.fastmcp import FastMCP  # noqa: E402

import store  # noqa: E402
from google_auth import service  # noqa: E402

mcp = FastMCP("watch-tools")


def get_json(url: str, headers: dict | None = None) -> dict:
    """Small helper for calling web APIs."""
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


# ================================================================ Gmail


@mcp.tool()
def gmail_search(query: str = "is:unread newer_than:1d", max_results: int = 10) -> str:
    """Search Luke's Gmail (read-only). Uses Gmail search syntax, e.g.
    'is:unread newer_than:1d', 'from:someone@x.com', 'subject:interview'.
    Returns sender, subject, date, and a short preview for each email.
    Use it for 'any important emails?' and then judge which ones matter."""
    gmail = service("gmail", "v1")
    found = gmail.users().messages().list(
        userId="me", q=query, maxResults=min(max_results, 25)
    ).execute()
    ids = [m["id"] for m in found.get("messages", [])]
    if not ids:
        return "No matching emails."
    out = []
    for mid in ids:
        msg = gmail.users().messages().get(
            userId="me", id=mid, format="metadata",
            metadataHeaders=["From", "Subject", "Date"],
        ).execute()
        h = {x["name"]: x["value"] for x in msg["payload"]["headers"]}
        out.append(
            f"- From: {h.get('From', '?')} | Subject: {h.get('Subject', '(none)')} "
            f"| {h.get('Date', '')}\n  {msg.get('snippet', '')[:200]}"
        )
    return "\n".join(out)


# ============================================================= Calendar


@mcp.tool()
def calendar_events(days_ahead: int = 1) -> str:
    """List Luke's Google Calendar events from now through the next N days."""
    cal = service("calendar", "v3")
    start = store.now()
    end = start.replace(hour=23, minute=59) + timedelta(days=max(days_ahead, 1) - 1)
    events = cal.events().list(
        calendarId="primary", timeMin=start.isoformat(), timeMax=end.isoformat(),
        singleEvents=True, orderBy="startTime", maxResults=25,
    ).execute().get("items", [])
    if not events:
        return "Nothing on the calendar."
    lines = []
    for e in events:
        when = e["start"].get("dateTime", e["start"].get("date"))
        lines.append(f"- {when}: {e.get('summary', '(no title)')}"
                     + (f" @ {e['location']}" if e.get("location") else ""))
    return "\n".join(lines)


@mcp.tool()
def calendar_add_event(title: str, start: str, duration_minutes: int = 60,
                       location: str = "") -> str:
    """Add an event to Luke's Google Calendar. `start` is local ISO time like
    '2026-10-06T15:30'. Always use this when Luke asks to add something to his calendar."""
    cal = service("calendar", "v3")
    begin = store.parse_time(start)
    body = {
        "summary": title,
        "start": {"dateTime": begin.isoformat()},
        "end": {"dateTime": (begin + timedelta(minutes=duration_minutes)).isoformat()},
    }
    if location:
        body["location"] = location
    cal.events().insert(calendarId="primary", body=body).execute()
    return f"Added '{title}' at {begin:%a %b %d %I:%M %p}."


# =============================================================== Sheets
# Spreadsheets are set in tools/.env, e.g.
#   SHEET_BUDGET=<spreadsheet id from the sheet's URL>
#   SHEET_BUDGET_TAB=Expenses
#   SHEET_LOG=<another spreadsheet id>   (optional, for hours/job apps)


def _sheet(name: str) -> tuple[str, str]:
    key = name.upper()
    sid = os.environ.get(f"SHEET_{key}")
    if not sid:
        raise RuntimeError(f"No sheet named '{name}'. Set SHEET_{key} in tools/.env.")
    return sid, os.environ.get(f"SHEET_{key}_TAB", "")


@mcp.tool()
def sheet_tabs(sheet: str = "budget") -> str:
    """List the tabs in one of Luke's spreadsheets ('budget' or 'log')."""
    sid, _ = _sheet(sheet)
    meta = service("sheets", "v4").spreadsheets().get(spreadsheetId=sid).execute()
    return ", ".join(s["properties"]["title"] for s in meta["sheets"])


@mcp.tool()
def sheet_read(sheet: str = "budget", tab: str = "", last_rows: int = 30) -> str:
    """Read a tab from one of Luke's spreadsheets ('budget' or 'log').
    Returns the header row plus the most recent rows. Read before adding a row
    so new rows match the columns, and to answer questions like
    'how much have I spent on food this month?'."""
    sid, default_tab = _sheet(sheet)
    rng = tab or default_tab or "A:Z"
    if tab or default_tab:
        rng = f"'{rng}'"
    rows = service("sheets", "v4").spreadsheets().values().get(
        spreadsheetId=sid, range=rng
    ).execute().get("values", [])
    if not rows:
        return "That tab is empty."
    header, body = rows[0], rows[1:]
    shown = body[-last_rows:]
    lines = [" | ".join(header)] + [" | ".join(r) for r in shown]
    note = f"(showing last {len(shown)} of {len(body)} rows)"
    return note + "\n" + "\n".join(lines)


@mcp.tool()
def sheet_add_row(values: list[str], sheet: str = "budget", tab: str = "") -> str:
    """Add one row to a tab in Luke's spreadsheets ('budget' or 'log').
    `values` must follow the tab's column order (call sheet_read first to see
    the headers). Dates as YYYY-MM-DD, money as plain numbers like 12.50."""
    sid, default_tab = _sheet(sheet)
    target = tab or default_tab or "Sheet1"
    service("sheets", "v4").spreadsheets().values().append(
        spreadsheetId=sid, range=f"'{target}'", valueInputOption="USER_ENTERED",
        insertDataOption="INSERT_ROWS", body={"values": [values]},
    ).execute()
    return f"Added to {sheet} / {target}: {', '.join(values)}"


# ============================================================ Reminders


@mcp.tool()
def add_reminder(text: str, when: str) -> str:
    """Set a reminder that buzzes on the watch. `when` is local ISO time like
    '2026-10-05T17:00'. Work out the exact time from phrases like 'in 20 minutes'
    or 'at 5' using the current time from the `current_time` tool."""
    dt = store.parse_time(when)
    if dt <= store.now():
        return "That time has already passed. Pick a future time."
    item = store.add_reminder(text, dt)
    return f"Reminder set for {dt:%a %I:%M %p}: {text} (id {item['id']})"


@mcp.tool()
def list_reminders() -> str:
    """List upcoming reminders that haven't buzzed yet."""
    items = store.upcoming_reminders()
    if not items:
        return "No upcoming reminders."
    return "\n".join(
        f"- {store.parse_time(i['when']):%a %I:%M %p}: {i['text']} (id {i['id']})"
        for i in items
    )


@mcp.tool()
def cancel_reminder(reminder_id: str) -> str:
    """Cancel a reminder by its id (get ids from list_reminders)."""
    return "Cancelled." if store.cancel_reminder(reminder_id) else "No reminder with that id."


@mcp.tool()
def current_time() -> str:
    """The current local date and time. Use before working out relative times."""
    return store.now().strftime("%A %Y-%m-%d %H:%M %Z")


# ========================================================== Weather/notes


@mcp.tool()
def get_weather(city: str = "Louisville") -> str:
    """Current weather and today's high/low for a city. Uses Open-Meteo (no API key)."""
    geo = get_json(
        "https://geocoding-api.open-meteo.com/v1/search?count=1&name="
        + urllib.parse.quote(city)
    )
    if not geo.get("results"):
        return f"Couldn't find {city}."
    place = geo["results"][0]
    w = get_json(
        "https://api.open-meteo.com/v1/forecast"
        f"?latitude={place['latitude']}&longitude={place['longitude']}"
        "&current=temperature_2m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&temperature_unit=fahrenheit&timezone=auto&forecast_days=1"
    )
    return (
        f"{place['name']}: {w['current']['temperature_2m']}°F now, "
        f"high {w['daily']['temperature_2m_max'][0]}°F, "
        f"low {w['daily']['temperature_2m_min'][0]}°F, "
        f"{w['daily']['precipitation_probability_max'][0]}% chance of rain."
    )


@mcp.tool()
def add_note(text: str) -> str:
    """Save a quick note or idea Luke dictates to the watch."""
    store.DATA_DIR.mkdir(parents=True, exist_ok=True)
    with store.NOTES.open("a") as f:
        f.write(f"{store.now():%Y-%m-%d %H:%M}  {text}\n")
    return "Saved."


@mcp.tool()
def read_notes(last: int = 5) -> str:
    """Read back Luke's most recent saved notes."""
    if not store.NOTES.exists():
        return "No notes yet."
    return "\n".join(store.NOTES.read_text().strip().splitlines()[-last:])


# ============================================================= template
# Copy this pattern for a new API that needs a key:
#
# @mcp.tool()
# def my_new_thing(query: str) -> str:
#     """Describe exactly when Claude should use this."""
#     key = os.environ["MY_API_KEY"]          # put MY_API_KEY=... in tools/.env
#     data = get_json(f"https://api.example.com/search?q={urllib.parse.quote(query)}",
#                     headers={"Authorization": f"Bearer {key}"})
#     return str(data)


if __name__ == "__main__":
    mcp.run()
