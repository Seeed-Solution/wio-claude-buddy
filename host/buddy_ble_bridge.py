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

The data-producing logic lives in buddy_common.py (shared with the SenseCraft
uplink bridge); this file is only the BLE transport.

Setup:
    python3 -m venv .venv && . .venv/bin/activate
    pip install bleak
Run (with the Wio powered on, showing "advertising", NOT paired in Claude Desktop):
    python3 buddy_ble_bridge.py
"""
from __future__ import annotations

import asyncio
import json
import sys
import time
from datetime import datetime
from pathlib import Path

# claude_meter (~/.claude parsing + usage math) is vendored alongside this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from claude_meter.scanner import IncrementalScanner          # noqa: E402
from claude_meter.pricing import caps_for                    # noqa: E402
from claude_meter.snapshot import build_snapshot             # noqa: E402

from buddy_common import (                                   # noqa: E402
    PROJECTS, WEEK_RETAIN_S, USAGE_POLL_S,
    load_creds, load_token, claude_code_ua, probe_usage, heartbeat,
)

from bleak import BleakScanner, BleakClient                  # noqa: E402

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # buddy's write characteristic
DEVICE_PREFIX = "Claude"
INTERVAL_S = 5


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
