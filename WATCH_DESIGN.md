# Claude Watch: Design Notes

Luke's plan for how the watch should look and work. Claude Code: read this
before writing or changing the firmware, and ask Luke before going against it.

Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.06 (410×502 AMOLED touch screen,
dual mics, speaker, BOOT + PWR buttons). Server endpoints: see `../README.md`.

---

## 1. The Claude button (BOOT button)

The **BOOT button** is the Claude button. One press starts listening, and a second press stops.

| Step | What Luke does | What the watch shows |
|------|----------------|----------------------|
| 1. Listening | Presses BOOT | An **orange ring/border lights up around the outer edge of the screen**, so he knows Claude is listening. The mic records. |
| 2. Sent | Presses BOOT again | Recording stops. The ring turns into a **moving animation** (e.g. spinning/pulsing orange) so he knows the request went to the server and Claude is working on it. |
| 3. Answer | Waits | **Orange fills the whole screen** and the answer appears **inside a box** in the middle. Long answers scroll. |

Details:
- Orange = Claude's color throughout.
- Tap the screen or press BOOT to dismiss the answer and go back.
- If something fails (no Wi-Fi, server down, timeout), show a short error in the same box instead of hanging.
- Note: the BOOT button is GPIO0. Holding it while powering on enters download mode. That's fine; just press it normally once the watch is running.

**Server work needed:** the server only accepts text right now (`POST /ask`).
Voice needs a new endpoint that accepts recorded audio, turns it into text
(speech-to-text on the Pi, e.g. whisper.cpp or faster-whisper), then asks
Claude as usual.

---

## 2. Screens and gestures

The **home screen** is the watch face (time, date). The timer and stopwatch Luke already has stay as they are.

| Gesture | Opens |
|---------|-------|
| Swipe **left → right** | **Calendar** |
| Swipe **up** | **Tasks** |
| Press **BOOT** (anywhere) | **Claude** (see section 1) |

Swiping back the opposite way returns to the watch face.

---

## 3. Calendar screen (swipe left → right)

**Luke's idea:** show a **picture of his Google Calendar** on the watch,
resized to fit the screen. When he changes something (for example, asks
Claude to add an event), the next swipe should **grab a fresh picture** so
it's up to date.

**Suggested way to build it:**
- The Pi makes the picture. A new server endpoint (e.g. `GET /calendar.png`)
  reads his Google Calendar (the Calendar tools already exist) and draws a
  clean image sized for the watch (410×502), with today and the next few
  days in big, readable text.
- Every time Luke swipes to the calendar, the watch downloads a fresh image,
  so changes made through Claude show up right away.
- This looks like a screenshot but is easier and more reliable than
  screenshotting the Google Calendar website, which would need a logged-in
  browser and would be hard to read when shrunk down.

Open question for Luke: day view, 3-day view, or week view?

---

## 4. Tasks screen (swipe up)

**Luke's idea:** a to-do list on the watch, **run by Claude**. Luke tells
Claude ("add buy cat food to my tasks", "mark the essay done"), and the
watch shows the list.

**Suggested way to build it:**
- Store tasks in **Google Tasks** (same Google login as Calendar/Gmail;
  needs the Tasks API enabled and the `tasks` scope added in
  `tools/google_auth.py`), or in a simple file on the Pi if Luke prefers.
- Add Claude tools: add task, list tasks, complete task, delete task.
- Add a server endpoint (e.g. `GET /tasks`) the watch calls when Luke
  swipes up.
- On the watch: a scrollable list. Tapping a task checks it off.

Open question for Luke: Google Tasks (also shows on his phone) or Pi-only?

---

## 5. Already planned (from earlier)

- iPhone notifications on the watch over Bluetooth (ANCS)
- Reminders that buzz (`GET /reminders/due`, poll every minute)
- Morning brief (`GET /brief`)
- Step counter (built-in motion sensor)

---

## Suggested build order

1. "Hello Luke" + time on screen (first flash)
2. Wi-Fi + typed/test question to `POST /ask` → orange answer screen
3. BOOT button flow: orange ring → animation → answer (with voice)
4. Swipe navigation + calendar screen
5. Tasks screen
6. Reminders, notifications, brief, steps
