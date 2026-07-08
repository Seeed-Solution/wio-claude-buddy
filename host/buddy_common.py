#!/usr/bin/env python3
"""Transport-agnostic core shared by the Claude Buddy host bridges.

Everything here derives data from ~/.claude (via the vendored claude_meter
package) and Claude's usage endpoint; nothing here knows how the result
reaches the Wio. buddy_ble_bridge.py (BLE/bleak) and
buddy_sensecraft_bridge.py (SenseCraft HTTPS uplink) both import from here.
"""
from __future__ import annotations

import json
import subprocess
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

PROJECTS = Path.home() / ".claude" / "projects"
CREDS = Path.home() / ".claude" / ".credentials.json"
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
