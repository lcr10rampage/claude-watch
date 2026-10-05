"""
Claude Watch server.

The watch talks to this server over Wi-Fi. For questions, the server runs
`claude -p` (Claude Code headless mode, using your Claude subscription) and
sends the answer back as JSON.

Endpoints (all except /health need the X-Watch-Token header):
  GET  /health             -> {"ok": true}
  POST /ask                -> body {"question": "..."} -> {"answer": "..."}
  GET  /brief              -> {"answer": "..."}  morning brief (weather, calendar, email)
  GET  /reminders/due      -> {"reminders": [...]}  reminders to buzz now (each returned once)
  GET  /reminders          -> {"reminders": [...]}  all upcoming reminders

The watch should call /reminders/due about once a minute.

Uses only the Python standard library, so there's nothing to pip install.
"""

import json
import os
import shlex
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import store  # noqa: E402  (shared with the watch tools)

HOST = os.environ.get("WATCH_HOST", "0.0.0.0")
PORT = int(os.environ.get("WATCH_PORT", "8787"))

# Shared secret so random devices on your Wi-Fi can't use your Claude account.
TOKEN = os.environ.get("WATCH_TOKEN", "")

# How long to wait for Claude before giving up (seconds).
TIMEOUT = int(os.environ.get("CLAUDE_TIMEOUT", "90"))

# Keeps answers short enough for a tiny screen.
WATCH_PROMPT = os.environ.get(
    "WATCH_SYSTEM_PROMPT",
    "You are answering on a small smartwatch screen. "
    "Reply in 1-3 short, plain sentences. No markdown, no lists, no emoji.",
)

BRIEF_PROMPT = os.environ.get(
    "WATCH_BRIEF_PROMPT",
    "Give my morning brief: today's weather, my calendar for today, and any "
    "important unread emails from the last day (skip promotions and newsletters). "
    "Keep it under 5 short sentences.",
)

# Connections Claude may use without asking (comma-separated).
ALLOWED_TOOLS = [
    t.strip() for t in os.environ.get("CLAUDE_ALLOWED_TOOLS", "mcp__watch-tools").split(",")
    if t.strip()
]

# Any other claude flags you want to pass through.
EXTRA_ARGS = shlex.split(os.environ.get("CLAUDE_EXTRA_ARGS", ""))

# Claude runs inside this folder, so it reads brain/CLAUDE.md every time.
BRAIN_DIR = os.environ.get("WATCH_BRAIN_DIR", str(ROOT / "brain"))

MAX_QUESTION_CHARS = 2000


def ask_claude(question: str) -> str:
    cmd = ["claude", "-p", question, "--output-format", "text",
           "--append-system-prompt", WATCH_PROMPT]
    if ALLOWED_TOOLS:
        cmd += ["--allowedTools", *ALLOWED_TOOLS]
    cmd += EXTRA_ARGS
    result = subprocess.run(cmd, capture_output=True, text=True,
                            timeout=TIMEOUT, cwd=BRAIN_DIR)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "claude exited with an error")
    return result.stdout.strip()


class Handler(BaseHTTPRequestHandler):
    def _send(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self) -> bool:
        return not TOKEN or self.headers.get("X-Watch-Token") == TOKEN

    def _answer(self, question: str) -> None:
        print(f"Q: {question}", flush=True)
        try:
            answer = ask_claude(question)
        except subprocess.TimeoutExpired:
            return self._send(504, {"error": "Claude took too long"})
        except FileNotFoundError:
            return self._send(500, {"error": "claude command not found; is Claude Code installed?"})
        except RuntimeError as e:
            return self._send(502, {"error": str(e)})
        print(f"A: {answer}", flush=True)
        self._send(200, {"answer": answer})

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, {"ok": True})
        if not self._authorized():
            return self._send(401, {"error": "bad token"})
        if self.path == "/brief":
            return self._answer(BRIEF_PROMPT)
        if self.path == "/reminders/due":
            return self._send(200, {"reminders": store.pop_due_reminders()})
        if self.path == "/reminders":
            return self._send(200, {"reminders": store.upcoming_reminders()})
        self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/ask":
            return self._send(404, {"error": "not found"})
        if not self._authorized():
            return self._send(401, {"error": "bad token"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length) or b"{}")
            question = str(data.get("question", "")).strip()
        except (ValueError, json.JSONDecodeError):
            return self._send(400, {"error": "send JSON like {\"question\": \"...\"}"})
        if not question:
            return self._send(400, {"error": "question is empty"})
        if len(question) > MAX_QUESTION_CHARS:
            return self._send(400, {"error": "question is too long"})
        self._answer(question)

    def log_message(self, fmt, *args):  # quieter default logging
        pass


def main():
    if not TOKEN:
        print("WARNING: WATCH_TOKEN is not set; anyone on your network can use this server.")
    print(f"Claude Watch server listening on http://{HOST}:{PORT}")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
