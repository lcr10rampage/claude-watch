"""
Test client: pretends to be the watch.

Usage:
  python3 ask.py "What's on my calendar today?"
  python3 ask.py --brief                 # morning brief
  python3 ask.py --due                   # reminders that should buzz now
  python3 ask.py --url http://raspberrypi.local:8787 "Hi"
"""

import argparse
import json
import os
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("question", nargs="?")
parser.add_argument("--brief", action="store_true")
parser.add_argument("--due", action="store_true")
parser.add_argument("--url", default=os.environ.get("WATCH_URL", "http://localhost:8787"))
parser.add_argument("--token", default=os.environ.get("WATCH_TOKEN", ""))
args = parser.parse_args()

base = args.url.rstrip("/")
headers = {"Content-Type": "application/json", "X-Watch-Token": args.token}

if args.brief:
    req = urllib.request.Request(base + "/brief", headers=headers)
elif args.due:
    req = urllib.request.Request(base + "/reminders/due", headers=headers)
elif args.question:
    req = urllib.request.Request(
        base + "/ask", headers=headers,
        data=json.dumps({"question": args.question}).encode(),
    )
else:
    parser.error("give a question, --brief, or --due")

try:
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read())
        print(data.get("answer") or json.dumps(data, indent=2))
except urllib.error.HTTPError as e:
    print(f"Error {e.code}: {e.read().decode()}")
