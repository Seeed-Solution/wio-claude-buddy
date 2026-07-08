#!/usr/bin/env python3
"""Unified BLE bridge for the Claude Buddy (Wio Terminal).

Reads ~/.claude (reusing the claude-usage meter modules), connects to the
"Claude Wio" device over Bluetooth with bleak, and sends BOTH:

  * a Hardware-Buddy-style session heartbeat  -> drives the pet + sessions panel
    {total, running, waiting, msg, entries, tokens, tokens_today}
  * the claude-usage v:1 plan-usage snapshot  -> drives the USAGE panel
    {v:1, session:{pct,reset_s}, week:{pct,reset_s,reset_label}, today:{tokens}, ...}

This REPLACES Claude Desktop's built-in Hardware Buddy (the buddy holds a single
BLE connection). Everything is derived from ~/.claude — so it can't show live
permission prompts (those live in the desktop app), but it adds real usage gauges.

Setup:
    python3 -m venv .venv && . .venv/bin/activate
    pip install bleak
    # (the claude-usage host package is imported from ../../claude-usage/host)
Run (with the Wio powered on, showing "advertising", NOT paired in Claude Desktop):
    python3 buddy_ble_bridge.py
"""
from __future__ import annotations

import asyncio
import json
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

# claude_meter (~/.claude parsing + usage math) is vendored alongside this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from claude_meter.scanner import IncrementalScanner          # noqa: E402
from claude_meter.pricing import caps_for                    # noqa: E402
from claude_meter.snapshot import build_snapshot             # noqa: E402

from bleak import BleakScanner, BleakClient                  # noqa: E402

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # buddy's write characteristic
DEVICE_PREFIX = "Claude"
PROJECTS = Path.home() / ".claude" / "projects"
CREDS = Path.home() / ".claude" / ".credentials.json"
INTERVAL_S = 5
WEEK_RETAIN_S = 8 * 86400

# Claude Code's own (undocumented) usage endpoint — exact session/weekly limits.
# Rate-limited: must send a claude-code User-Agent and poll infrequently.
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
USAGE_POLL_S = 60


def _oauth() -> dict:
    """Live OAuth creds. Claude Code on macOS keeps the current (refreshed)
    token in the Keychain; ~/.claude/.credentials.json is often stale/expired.
    Prefer the Keychain, fall back to the file."""
    try:
        r = subprocess.run(
            ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
            capture_output=True, text=True, timeout=10)
        if r.returncode == 0 and r.stdout.strip():
            d = json.loads(r.stdout.strip())
            return d.get("claudeAiOauth", d)
    except Exception:
        pass
    try:
        d = json.loads(CREDS.read_text())
        return d.get("claudeAiOauth", d)
    except Exception:
        return {}


def load_creds() -> dict:
    o = _oauth()
    return {"subscriptionType": o.get("subscriptionType", ""),
            "rateLimitTier": o.get("rateLimitTier", "")}


def load_token() -> str:
    return _oauth().get("accessToken", "")


def claude_code_ua() -> str:
    try:
        out = subprocess.run(["claude", "--version"], capture_output=True,
                             text=True, timeout=5).stdout.strip().split()
        ver = out[0] if out else "2.0.0"
    except Exception:
        ver = "2.0.0"
    return f"claude-code/{ver}"


def _conv_window(o: "dict | None", want_label: bool) -> "dict | None":
    """Convert one usage window {utilization(0-100), resets_at ISO} -> our shape."""
    if not o or o.get("utilization") is None:
        return None
    pct = round(float(o["utilization"]))
    reset_s, label = 0, ""
    ra = o.get("resets_at")
    if ra:
        try:
            t = datetime.fromisoformat(ra.replace("Z", "+00:00"))
            reset_s = max(0, int((t - datetime.now(timezone.utc)).total_seconds()))
            if want_label:
                label = t.astimezone().strftime("%a %H:%M")     # e.g. "Fri 01:00"
        except ValueError:
            pass
    out = {"pct": pct, "reset_s": reset_s}
    if want_label:
        out["label"] = label
    return out


def probe_usage(token: str, ua: str) -> "dict | None":
    """Call the real usage endpoint. Returns {session, week} or None on failure."""
    if not token:
        return None
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "User-Agent": ua,
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            d = json.loads(r.read())
    except Exception as e:
        print(f"usage probe failed ({e}); using local estimate")
        return None
    return {"session": _conv_window(d.get("five_hour"), False),
            "week": _conv_window(d.get("seven_day"), True)}


def heartbeat(events, now: float) -> dict:
    """Derive a Hardware-Buddy-style heartbeat from usage events."""
    recent = {e.session_id for e in events if now - e.ts < 300}        # active last 5 min
    win = {e.session_id for e in events if now - e.ts < 5 * 3600}      # last 5 h
    today = datetime.fromtimestamp(now).date()
    today_tok = sum(e.output_tokens for e in events
                    if datetime.fromtimestamp(e.ts).date() == today)
    running = len(recent)
    last = sorted(events, key=lambda e: e.ts, reverse=True)[:4]
    entries = [f"{datetime.fromtimestamp(e.ts):%H:%M} "
               f"{(e.model or 'msg').split('-')[-1]}" for e in last]
    return {
        "total": max(running, len(win)),
        "running": running,
        "waiting": 0,
        "msg": (f"{running} active" if running else "idle"),
        "entries": entries,
        "tokens": int(today_tok),
        "tokens_today": int(today_tok),
    }


async def send_line(client: BleakClient, obj: dict) -> None:
    """Write one newline-terminated JSON object, chunked to fit BLE writes.
    The buddy reassembles by newline, so long snapshots split across writes."""
    data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
    for i in range(0, len(data), 160):
        await client.write_gatt_char(NUS_RX, data[i:i + 160], response=False)
        await asyncio.sleep(0.02)


async def session(dev) -> None:
    tz = datetime.now().astimezone().tzinfo
    creds = load_creds()
    caps = caps_for(creds["subscriptionType"], creds["rateLimitTier"])
    token, ua = load_token(), claude_code_ua()
    scanner = IncrementalScanner(PROJECTS)
    events: list = []
    probed = None
    last_probe = 0.0
    async with BleakClient(dev) as client:
        print("connected to", dev.name)
        off = int(datetime.now(tz).utcoffset().total_seconds())
        await send_line(client, {"time": [int(time.time()), off]})
        while client.is_connected:
            now = time.time()
            events.extend(scanner.scan())
            events = [e for e in events if e.ts >= now - WEEK_RETAIN_S]   # bound memory
            snap = build_snapshot(events, now, caps, tz, None, "ok")

            # Once a minute, fetch the EXACT limits from Claude's usage endpoint
            # and override the local estimate (kept as fallback if it fails).
            if now - last_probe >= USAGE_POLL_S:
                last_probe = now
                p = probe_usage(token, ua)
                if p:
                    probed = p
            if probed:
                if probed["session"]:
                    snap["session"].update(probed["session"]); snap["session"]["src"] = "exact"
                if probed["week"]:
                    w = probed["week"]
                    snap["week"]["pct"] = w["pct"]; snap["week"]["reset_s"] = w["reset_s"]
                    snap["week"]["reset_label"] = w.get("label", ""); snap["week"]["src"] = "exact"

            await send_line(client, snap)                     # v:1 usage
            hb = heartbeat(events, now)
            await send_line(client, hb)                       # pet / sessions
            src = snap["session"].get("src", "est")
            print(f"sent ({src}): session {snap['session']['pct']}%  "
                  f"week {snap['week']['pct']}%  running {hb['running']}")
            await asyncio.sleep(INTERVAL_S)
    print("disconnected")


async def run() -> None:
    while True:
        print("scanning for 'Claude Wio' (power it on; do not pair it in Claude Desktop)...")
        # On macOS, CoreBluetooth leaves BLEDevice.name None for a never-paired
        # device; the advertised name only arrives in ad.local_name. Check both.
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: (d.name or ad.local_name or "").startswith(DEVICE_PREFIX),
            timeout=20)
        if not dev:
            print("not found; retrying in 5s")
            await asyncio.sleep(5)
            continue
        try:
            await session(dev)
        except Exception as e:
            print(f"session ended: {e}")
        # The buddy stops advertising after a disconnect; if reconnect fails,
        # reboot the Wio. We keep retrying in case it comes back.
        await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        print("\nbye")
