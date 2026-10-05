"""
One Google login for Gmail, Calendar, and Sheets.

First-time setup (run once on the Pi, with its desktop/browser):
  1. In Google Cloud Console, enable the Gmail, Google Calendar, and
     Google Sheets APIs for your project.
  2. Create an OAuth client ID of type "Desktop app", download the JSON,
     and save it as tools/credentials.json
  3. Run:  .venv/bin/python google_auth.py
     A browser opens; sign in and allow access. This creates tools/token.json.

After that, the watch tools log in automatically and refresh the token
themselves. Never commit credentials.json or token.json (they're gitignored).
"""

from pathlib import Path

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from googleapiclient.discovery import build

HERE = Path(__file__).parent
CREDENTIALS = HERE / "credentials.json"
TOKEN = HERE / "token.json"

# Gmail is read-only on purpose: the watch can read email but never send it.
SCOPES = [
    "https://www.googleapis.com/auth/gmail.readonly",
    "https://www.googleapis.com/auth/calendar",
    "https://www.googleapis.com/auth/spreadsheets",
]


def get_creds() -> Credentials:
    if not TOKEN.exists():
        raise RuntimeError(
            "Google isn't connected yet. On the Pi run: "
            "cd ~/claude-watch/tools && .venv/bin/python google_auth.py"
        )
    creds = Credentials.from_authorized_user_file(str(TOKEN), SCOPES)
    if not creds.valid and creds.expired and creds.refresh_token:
        creds.refresh(Request())
        TOKEN.write_text(creds.to_json())
    return creds


def service(name: str, version: str):
    return build(name, version, credentials=get_creds(), cache_discovery=False)


def login() -> None:
    from google_auth_oauthlib.flow import InstalledAppFlow

    if not CREDENTIALS.exists():
        raise SystemExit(f"Missing {CREDENTIALS}. Download it from Google Cloud first.")
    flow = InstalledAppFlow.from_client_secrets_file(str(CREDENTIALS), SCOPES)
    creds = flow.run_local_server(port=0)
    TOKEN.write_text(creds.to_json())
    print(f"Connected! Saved {TOKEN}")


if __name__ == "__main__":
    login()
