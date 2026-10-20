# Claude Watch

A homemade smartwatch powered by Claude. The watch (Waveshare ESP32-S3
Touch AMOLED 2.06) is just a screen, mic, and buttons; the thinking happens
on a Raspberry Pi running Claude Code.

```
Watch ──Wi-Fi──▶ server/server.py (Pi) ──▶ claude -p ──▶ tools/watch_tools.py
                                                          ├─ Gmail (read-only)
                                                          ├─ Google Calendar
                                                          ├─ Google Sheets (budget)
                                                          ├─ Reminders
                                                          └─ Weather, notes
```

## Folders

| Folder      | What it is                                                     |
|-------------|----------------------------------------------------------------|
| `server/`   | Receives requests from the watch, asks Claude, returns answers |
| `brain/`    | `CLAUDE.md`: how the watch's Claude behaves (edit freely)       |
| `tools/`    | Your APIs, one Python function each                             |
| `data/`     | Reminders and notes (created automatically, not in git)         |
| `firmware/` | Watch code (coming once the watch arrives)                      |

## What you can ask

- "What's on my calendar today?" / "Add dentist Tuesday at 3:30"
- "Any important emails?"
- "Spent 12 on lunch" / "How much have I spent on food this month?"
- "Remind me at 5 to email the client" (the watch buzzes)
- Tap **Brief** for weather + calendar + important email

## Setup on the Pi

1. **Get the code**
   ```bash
   cd ~ && git clone https://github.com/lcr10rampage/claude-watch.git
   cd claude-watch
   ```

2. **Install the tools**
   ```bash
   cd ~/claude-watch/tools
   python3 -m venv .venv
   .venv/bin/pip install -r requirements.txt
   cp .env.example .env
   ```

3. **Connect Google (Gmail, Calendar, Sheets)**
   1. Go to [console.cloud.google.com](https://console.cloud.google.com), pick
      or create a project.
   2. **APIs & Services → Library**: enable **Gmail API**, **Google Calendar
      API**, and **Google Sheets API**.
   3. **APIs & Services → OAuth consent screen**: choose External, fill in
      the app name and your email, and add yourself under **Test users**.
   4. **Credentials → Create credentials → OAuth client ID → Desktop app**.
      Download the JSON and save it as `~/claude-watch/tools/credentials.json`.
   5. Log in once (a browser opens on the Pi):
      ```bash
      cd ~/claude-watch/tools && .venv/bin/python google_auth.py
      ```

4. **Point it at your budget sheet.** Open `tools/.env` and set
   `SHEET_BUDGET` to the ID from your sheet's URL
   (`docs.google.com/spreadsheets/d/<ID>/edit`) and `SHEET_BUDGET_TAB` to
   the tab where expenses go.

5. **Register the tools with Claude Code**
   ```bash
   claude mcp add --scope user watch-tools -- ~/claude-watch/tools/.venv/bin/python ~/claude-watch/tools/watch_tools.py
   ```

6. **Configure the server**
   ```bash
   cd ~/claude-watch/server
   cp .env.example .env
   python3 -c "import secrets; print(secrets.token_hex(16))"   # paste into WATCH_TOKEN
   nano .env
   ```

7. **Run it and test**
   ```bash
   set -a; source .env; set +a
   python3 server.py
   # in a second terminal (use your token):
   python3 ask.py --token <token> "What's on my calendar today?"
   python3 ask.py --token <token> "Spent 5 on coffee"
   python3 ask.py --token <token> --brief
   ```

8. **Start it on boot**: either the system unit in `server/claude-watch.service`
   (edit the username and paths, then `sudo cp` + `sudo systemctl enable --now
   claude-watch`), or without sudo as a user unit: copy it to
   `~/.config/systemd/user/`, delete the `User=` line, change `WantedBy` to
   `default.target`, then `systemctl --user enable --now claude-watch` and
   `loginctl enable-linger $USER` so it runs with nobody logged in.

## Using the watch away from home (Tailscale Funnel)

Home routers behind carrier-grade NAT (Verizon 5G Home, for one) can't port
forward, so the Pi dials out instead. Tailscale Funnel gives it a permanent
public HTTPS name with nothing else to configure:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up                     # sign in via the printed link
sudo tailscale funnel --bg 8787       # prints https://<pi>.<tailnet>.ts.net
sudo tailscale set --operator=$USER   # lets you run tailscale commands without sudo
curl https://<pi>.<tailnet>.ts.net/health   # {"ok": true}
```

MagicDNS and HTTPS Certificates must be on in the Tailscale admin console
(DNS tab). The first certificate can fail for a while with
`set-dns ... 500 failed to create DNS record` (Tailscale-side, see
tailscale/tailscale#19942); it cleared on its own within the hour here.
Funnel and tailscaled restart on boot by themselves. Put the URL in the
watch's `main/secrets.h` as `WATCH_SERVER_URL`; the `X-Watch-Token` header is
the only thing guarding the endpoint, so keep the token long and private.

## Adding a new API

1. Add a function to `tools/watch_tools.py` (copy the template at the bottom).
2. Put any API key in `tools/.env`.
3. Add a line about it to `brain/CLAUDE.md` so Claude knows when to use it.

## Server endpoints (for the watch firmware)

All except `/health` need the header `X-Watch-Token: <token>`.

| Request              | Does                                                    |
|----------------------|---------------------------------------------------------|
| `POST /ask`          | Body `{"question": "..."}` → `{"answer": "..."}`         |
| `GET /brief`         | Morning brief → `{"answer": "..."}`                      |
| `GET /reminders/due` | Reminders to buzz now (each returned once). Poll every minute. |
| `GET /reminders`     | All upcoming reminders                                  |
| `GET /health`        | `{"ok": true}`                                          |

iPhone notifications don't go through the server: the watch gets them
straight from the phone over Bluetooth (ANCS). That's part of the firmware.
