# Claude Watch

A homemade smartwatch powered by Claude. The watch is just a screen, mic, and
button; the thinking happens on a Raspberry Pi running Claude Code.

```
Watch ──Wi-Fi──▶ server/server.py (Pi) ──▶ claude -p ──▶ Google Calendar
                                                     └──▶ tools/watch_tools.py (your APIs)
```

## Folders

| Folder      | What it is                                                    |
|-------------|---------------------------------------------------------------|
| `server/`   | Receives questions from the watch, asks Claude, returns answers |
| `brain/`    | `CLAUDE.md`: how the watch's Claude behaves (edit freely)       |
| `tools/`    | Your custom APIs, one Python function each                     |
| `firmware/` | Watch code (coming once the watch is picked)                   |

## Setup on the Pi

1. **Get the code**
   ```bash
   cd ~ && git clone https://github.com/<you>/claude-watch.git
   cd claude-watch
   ```

2. **Make sure Claude Code works** (logged in with your subscription)
   ```bash
   claude -p "say hi"
   ```

3. **Connect Google Calendar** to Claude Code. Add a Google Calendar MCP
   server named `google-calendar` with `claude mcp add` (ask Claude Code to
   help you set one up), then test:
   ```bash
   cd brain && claude -p "What's on my calendar today?"
   ```

4. **Set up your custom tools**
   ```bash
   cd ~/claude-watch/tools
   python3 -m venv .venv
   .venv/bin/pip install -r requirements.txt
   cp .env.example .env
   claude mcp add --scope user watch-tools -- ~/claude-watch/tools/.venv/bin/python ~/claude-watch/tools/watch_tools.py
   ```

5. **Configure the server**
   ```bash
   cd ~/claude-watch/server
   cp .env.example .env
   python3 -c "import secrets; print(secrets.token_hex(16))"   # paste into WATCH_TOKEN
   nano .env
   ```

6. **Run it and test**
   ```bash
   set -a; source .env; set +a
   python3 server.py
   # in a second terminal:
   python3 ask.py --token <your token> "What's the weather?"
   ```

7. **Start it on boot** (optional): see the comments in
   `server/claude-watch.service`.

## Adding a new API

1. Add a function to `tools/watch_tools.py` (copy the template at the bottom).
2. Put any API key in `tools/.env`.
3. Add a line about it to `brain/CLAUDE.md` so Claude knows it exists.
4. If it's a separate MCP server (not in `watch_tools.py`), add it with
   `claude mcp add` and append its name to `CLAUDE_ALLOWED_TOOLS` in
   `server/.env`.

## Talking to the server (for the watch firmware)

```
POST /ask
Headers: Content-Type: application/json, X-Watch-Token: <token>
Body:    {"question": "What's next on my calendar?"}
Reply:   {"answer": "Dentist at 3:30 PM today."}
```

`GET /health` returns `{"ok": true}`.
