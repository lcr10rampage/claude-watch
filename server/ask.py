"""
Test client: pretends to be the watch.

Usage:
  python3 ask.py "What's on my calendar today?"
  python3 ask.py --url http://raspberrypi.local:8787 "Hi"
"""

import argparse
import json
import os
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("question")
parser.add_argument("--url", default=os.environ.get("WATCH_URL", "http://localhost:8787"))
parser.add_argument("--token", default=os.environ.get("WATCH_TOKEN", ""))
args = parser.parse_args()

req = urllib.request.Request(
    args.url.rstrip("/") + "/ask",
    data=json.dumps({"question": args.question}).encode(),
    headers={"Content-Type": "application/json", "X-Watch-Token": args.token},
)

try:
    with urllib.request.urlopen(req, timeout=120) as resp:
        print(json.loads(resp.read())["answer"])
except urllib.error.HTTPError as e:
    print(f"Error {e.code}: {e.read().decode()}")
