"""
Shared storage for reminders and notes, used by both the watch tools
(when Claude adds a reminder) and the server (when the watch asks what's due).
Data lives in claude-watch/data/ (gitignored).
"""

import fcntl
import json
import os
import uuid
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

DATA_DIR = Path(os.environ.get("WATCH_DATA_DIR", Path(__file__).parent.parent / "data"))
REMINDERS = DATA_DIR / "reminders.json"
NOTES = DATA_DIR / "notes.txt"
TZ = ZoneInfo(os.environ.get("WATCH_TZ", "America/New_York"))


def now() -> datetime:
    return datetime.now(TZ)


def parse_time(text: str) -> datetime:
    """Accepts ISO like 2026-10-05T17:00 (local time assumed if no offset)."""
    dt = datetime.fromisoformat(text)
    return dt if dt.tzinfo else dt.replace(tzinfo=TZ)


@contextmanager
def _locked_reminders():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    lock = open(DATA_DIR / ".reminders.lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
        items = json.loads(REMINDERS.read_text()) if REMINDERS.exists() else []
        yield items
        REMINDERS.write_text(json.dumps(items, indent=2))
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


def add_reminder(text: str, when: datetime) -> dict:
    item = {
        "id": uuid.uuid4().hex[:8],
        "text": text,
        "when": when.isoformat(),
        "delivered": False,
    }
    with _locked_reminders() as items:
        items.append(item)
    return item


def upcoming_reminders() -> list[dict]:
    with _locked_reminders() as items:
        pending = [i for i in items if not i["delivered"]]
    return sorted(pending, key=lambda i: i["when"])


def cancel_reminder(reminder_id: str) -> bool:
    with _locked_reminders() as items:
        before = len(items)
        items[:] = [i for i in items if i["id"] != reminder_id]
        return len(items) < before


def pop_due_reminders() -> list[dict]:
    """Reminders whose time has come; marks them delivered so they buzz once."""
    current = now()
    due = []
    with _locked_reminders() as items:
        for i in items:
            if not i["delivered"] and parse_time(i["when"]) <= current:
                i["delivered"] = True
                due.append({"id": i["id"], "text": i["text"], "when": i["when"]})
        # keep the file small: drop delivered reminders older than a week
        items[:] = [
            i for i in items
            if not i["delivered"] or (current - parse_time(i["when"])).days < 7
        ]
    return due
