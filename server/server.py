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
  POST /ask-voice          -> body = 16 kHz mono 16-bit PCM (raw or WAV) -> {"heard", "answer", "retry"}
                              X-Watch-Context: calendar -> Claude adds the event, reply carries "event"
                              X-Watch-Context: tasks    -> cleaned task title in "task" (no Claude call)
                              speech-to-text runs here with faster-whisper (STT_MODEL, default tiny.en)

The watch should call /reminders/due about once a minute.

Only the standard library is needed for text questions; /ask-voice needs faster-whisper
(pip install faster-whisper in ~/stt-env and run the server with that python).
"""

import json
import math
import os
import shlex
import struct
import subprocess
import sys
import threading
import time
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
    "Reply in 1-3 short, plain sentences. No markdown, no lists, no emoji. "
    "Never append sources, citations, links or URLs, even after a web search; just state the answer.",
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

# ---- speech-to-text (voice questions from the watch) ----
STT_MODEL = os.environ.get("STT_MODEL", "tiny.en")
STT_THREADS = int(os.environ.get("STT_THREADS", "4"))
STT_RATE = 16000
MAX_AUDIO_BYTES = 2 * 1024 * 1024          # ~65 s of 16 kHz mono
SILENCE_DBFS = float(os.environ.get("STT_SILENCE_DBFS", "-45"))   # quieter than this = nobody spoke
STT_PROMPT = os.environ.get("STT_PROMPT", "Claude, Luke.")          # spelling hints for names whisper fumbles
STT_MIN_LOGPROB = float(os.environ.get("STT_MIN_LOGPROB", "-0.9"))  # below this whisper was guessing: ask again
STT_BEAM = int(os.environ.get("STT_BEAM", "3"))                      # 3: tiny.en stops truncating with a prompt
RETRY_TEXT = "Say that again?"
CALENDAR_PROMPT = (
    "Luke said this into his watch while looking at his calendar, so treat it as something to ADD to his "
    "Google Calendar: create the event now with the Google Calendar tools. Today is {today} ({weekday}); "
    "resolve words like tomorrow or Friday from that. Default duration one hour; if no time was given make "
    "it an all-day event. If the words are too garbled to know what to add, create nothing and reply with "
    f"exactly '{RETRY_TEXT}'. Otherwise reply with one short confirming sentence, then on a final separate "
    "line write exactly: EVENT: YYYY-MM-DD|<start time like 4:00 PM, or All day>|<event title>"
)
TASK_FILLERS = ("add a task", "add task", "add to my tasks", "add to my list", "new task", "remind me to",
                "add", "task", "please", "claude", "okay", "ok", "hey")
VOICE_PROMPT = (
    "This message was transcribed from speech by the watch's microphone, so a word or two may be wrong. "
    "If it reads as sensible English, respond to it normally, even if it is a statement or small talk rather "
    "than a question. Only if the words are garbled, cut off mid-thought, or you genuinely cannot tell what "
    f"Luke meant, do not guess: reply with exactly '{RETRY_TEXT}' and nothing else."
)
LAST_CLIP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "last_voice.wav")

_stt_model = None
_stt_lock = threading.Lock()


def _stt():
    """Load whisper once and keep it (first load ~2 s for tiny.en, longer the very first time)."""
    global _stt_model
    with _stt_lock:
        if _stt_model is None:
            from faster_whisper import WhisperModel   # lives in ~/stt-env; run the server with that python
            t = time.time()
            _stt_model = WhisperModel(STT_MODEL, device="cpu", compute_type="int8", cpu_threads=STT_THREADS)
            print(f"STT: loaded {STT_MODEL} in {time.time() - t:.1f}s", flush=True)
        return _stt_model


def pcm_from_body(data: bytes) -> bytes:
    """Accept raw PCM or a WAV; return raw 16-bit little-endian mono samples."""
    if data[:4] == b"RIFF" and b"data" in data[:64]:
        i = data.index(b"data", 12) + 8
        return data[i:]
    return data


def loudness_dbfs(pcm: bytes) -> float:
    n = len(pcm) // 2
    if n == 0:
        return -120.0
    samples = struct.unpack(f"<{n}h", pcm[: n * 2])
    rms = math.sqrt(sum(x * x for x in samples) / n)
    return 20 * math.log10(rms / 32768 + 1e-9)


def transcribe(pcm: bytes) -> tuple[str, float]:
    """Returns (text, confidence) where confidence is whisper's duration-weighted avg log-prob
    (about -0.3 for clean speech, -0.6 for so-so, below -0.9 it was guessing)."""
    import numpy as np
    audio = np.frombuffer(pcm[: (len(pcm) // 2) * 2], dtype=np.int16).astype(np.float32) / 32768.0
    segs, _ = _stt().transcribe(audio, beam_size=STT_BEAM, language="en", vad_filter=True,
                                condition_on_previous_text=False, initial_prompt=STT_PROMPT or None)
    segs = list(segs)
    total = sum(s.end - s.start for s in segs) or 1e-6
    conf = sum(s.avg_logprob * (s.end - s.start) for s in segs) / total if segs else -9.0
    return " ".join(s.text.strip() for s in segs).strip(), conf


def save_clip(pcm: bytes) -> None:
    try:
        import wave
        os.makedirs(os.path.dirname(LAST_CLIP), exist_ok=True)
        with wave.open(LAST_CLIP, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(STT_RATE); w.writeframes(pcm)
    except OSError:
        pass




def ask_claude(question: str, voice: bool = False, extra_prompt: str = "") -> str:
    cmd = ["claude", "-p", question, "--output-format", "text",
           "--append-system-prompt",
           WATCH_PROMPT + (" " + VOICE_PROMPT if voice else "") + (" " + extra_prompt if extra_prompt else "")]
    if ALLOWED_TOOLS:
        cmd += ["--allowedTools", *ALLOWED_TOOLS]
    cmd += EXTRA_ARGS
    result = subprocess.run(cmd, capture_output=True, text=True,
                            timeout=TIMEOUT, cwd=BRAIN_DIR)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "claude exited with an error")
    return strip_sources(result.stdout.strip())


def task_title(heard: str) -> str:
    """'Okay, add a task: buy cat food.' -> 'Buy cat food'"""
    import re
    t = heard.strip().strip(".!?,").strip()
    low = t.lower()
    changed = True
    while changed:
        changed = False
        for f in TASK_FILLERS:
            if low.startswith(f + " ") or low.startswith(f + ",") or low.startswith(f + ":"):
                t = t[len(f):].lstrip(" ,:"); low = t.lower(); changed = True
    t = re.sub(r"^(to|that i need to|i need to|i have to)\s+", "", t, flags=re.IGNORECASE)
    return (t[:1].upper() + t[1:]).strip(" .") if t else ""


def add_event(heard: str) -> tuple[str, dict | None]:
    """Have Claude create the event; returns (answer, {date,time,title} or None)."""
    import datetime, re
    now = datetime.datetime.now()
    prompt = CALENDAR_PROMPT.format(today=now.strftime("%Y-%m-%d"), weekday=now.strftime("%A"))
    raw = ask_claude(heard, voice=True, extra_prompt=prompt)
    m = re.search(r"EVENT:\s*(\d{4}-\d{2}-\d{2})\|([^|\n]+)\|([^\n]+)", raw)
    answer = re.sub(r"\n?\s*EVENT:.*$", "", raw, flags=re.DOTALL).strip() or "Added."
    if not m:
        return answer, None
    return answer, {"date": m.group(1), "time": m.group(2).strip(), "title": m.group(3).strip()}


def strip_sources(text: str) -> str:
    """Drop a trailing 'Sources:' block and any markdown links; the watch can't use them."""
    import re
    cut = re.search(r"\n\s*(sources?|references?)\s*:", text, re.IGNORECASE)
    if cut:
        text = text[: cut.start()].rstrip()
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)          # [label](url) -> label
    text = re.sub(r"https?://\S+", "", text)
    return text.strip()


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
        if self.path == "/ask-voice":
            return self.do_ask_voice()
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

    def do_ask_voice(self):
        if not self._authorized():
            return self._send(401, {"error": "bad token"})
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_AUDIO_BYTES:
            return self._send(400, {"error": f"send 16 kHz mono 16-bit PCM, up to {MAX_AUDIO_BYTES} bytes"})
        pcm = pcm_from_body(self.rfile.read(length))
        secs = len(pcm) / 2 / STT_RATE
        db = loudness_dbfs(pcm)
        save_clip(pcm)
        print(f"VOICE: {secs:.1f}s, {db:.0f} dBFS", flush=True)
        if db < SILENCE_DBFS:
            return self._send(200, {"heard": "", "answer": "I didn't hear anything. Try again a little closer."})
        try:
            t = time.time()
            heard, conf = transcribe(pcm)
            print(f"HEARD ({time.time() - t:.1f}s, conf {conf:.2f}): {heard}", flush=True)
        except ImportError:
            return self._send(503, {"error": "speech-to-text isn't installed on the Pi (faster-whisper)"})
        except Exception as e:  # noqa: BLE001 - report, don't crash the server
            return self._send(500, {"error": f"speech-to-text failed: {e}"})
        if not heard:
            return self._send(200, {"heard": "", "answer": "I couldn't make that out. Try again.", "retry": True})
        if conf < STT_MIN_LOGPROB:
            return self._send(200, {"heard": heard, "answer": RETRY_TEXT, "retry": True})
        mode = (self.headers.get("X-Watch-Context") or "").strip().lower()
        if mode == "tasks":
            title = task_title(heard)
            if not title:
                return self._send(200, {"heard": heard, "answer": RETRY_TEXT, "retry": True})
            print(f"TASK: {title}", flush=True)
            return self._send(200, {"heard": heard, "answer": f"Added: {title}", "task": title, "retry": False})
        if mode == "calendar":
            try:
                answer, event = add_event(heard)
            except subprocess.TimeoutExpired:
                return self._send(504, {"heard": heard, "error": "Claude took too long"})
            except (FileNotFoundError, RuntimeError) as e:
                return self._send(502, {"heard": heard, "error": str(e)})
            retry = answer.strip().startswith(RETRY_TEXT)
            print(f"EVENT: {event}  A: {answer}", flush=True)
            payload = {"heard": heard, "answer": RETRY_TEXT if retry else answer, "retry": retry}
            if event: payload["event"] = event
            return self._send(200, payload)
        try:
            answer = ask_claude(heard, voice=True)
        except subprocess.TimeoutExpired:
            return self._send(504, {"heard": heard, "error": "Claude took too long"})
        except (FileNotFoundError, RuntimeError) as e:
            return self._send(502, {"heard": heard, "error": str(e)})
        print(f"A: {answer}", flush=True)
        retry = answer.strip().strip('"\'').rstrip('.').lower() == RETRY_TEXT.rstrip('?').lower() or answer.strip().startswith(RETRY_TEXT)
        self._send(200, {"heard": heard, "answer": RETRY_TEXT if retry else answer, "retry": retry})

    def log_message(self, fmt, *args):  # quieter default logging
        pass


def _safe_preload():
    try:
        _stt()
    except Exception as e:  # noqa: BLE001
        print(f"STT: not available ({e}); /ask-voice will return 503", flush=True)


def main():
    if not TOKEN:
        print("WARNING: WATCH_TOKEN is not set; anyone on your network can use this server.")
    print(f"Claude Watch server listening on http://{HOST}:{PORT}")
    threading.Thread(target=_safe_preload, daemon=True).start()
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
