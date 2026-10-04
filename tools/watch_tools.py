"""
Your custom tools for the watch.

Every function marked @mcp.tool() becomes something Claude can use.
To connect a new API:
  1. Write a function below that calls the API and returns text.
  2. Give it a clear docstring (Claude reads it to decide when to use it).
  3. Restart nothing: Claude starts this file fresh on every question.

Register this file with Claude Code once (see README):
  claude mcp add watch-tools -- /home/pi/claude-watch/tools/.venv/bin/python /home/pi/claude-watch/tools/watch_tools.py

API keys go in tools/.env (never in this file), and are read with os.environ.
"""

import json
import os
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

from mcp.server.fastmcp import FastMCP

# Load API keys from tools/.env
_env = Path(__file__).with_name(".env")
if _env.exists():
    for line in _env.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, _, value = line.partition("=")
            os.environ.setdefault(key.strip(), value.strip())

mcp = FastMCP("watch-tools")


def get_json(url: str, headers: dict | None = None) -> dict:
    """Small helper for calling web APIs."""
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


# ---------------------------------------------------------------- examples


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
    """Save a quick note or idea the user dictates to the watch."""
    notes = Path(__file__).with_name("notes.txt")
    with notes.open("a") as f:
        f.write(f"{datetime.now():%Y-%m-%d %H:%M}  {text}\n")
    return "Saved."


@mcp.tool()
def read_notes(last: int = 5) -> str:
    """Read back the user's most recent saved notes."""
    notes = Path(__file__).with_name("notes.txt")
    if not notes.exists():
        return "No notes yet."
    lines = notes.read_text().strip().splitlines()
    return "\n".join(lines[-last:])


# ------------------------------------------------------------ template
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
