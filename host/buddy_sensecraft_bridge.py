#!/usr/bin/env python3
"""SenseCraft uplink bridge for the Claude Buddy (Wio Terminal).

Wireless replacement for buddy_ble_bridge.py: instead of writing newline-JSON
over BLE, this flattens the same heartbeat + v:1 usage snapshot into numeric
measurements and POSTs them to the SenseCraft Data Platform as a "Blank
Device" devkit. The Wio (NET firmware build) subscribes to the platform's
OpenStream MQTT broker and reassembles the same JSON on-device.

Provisioning (once, with sensecraft-cli authenticated):
    sensecraft-cli device devkit create --sku blank_device --name claude-buddy
    sensecraft-cli device devkit key --eui <EUI>

Config: env vars SENSECRAFT_API_BASE / SENSECRAFT_DEVICE_EUI /
SENSECRAFT_DEVICE_KEY / BUDDY_INTERVAL_S, falling back to
host/sensecraft_config.json (see sensecraft_config.example.json). A
"devices" list in the config file uplinks the same measurements to several
devices (e.g. Wio Terminal + SenseCAP Indicator) from this one process;
the legacy single "eui"/"key" form still works.

Run:
    python3 buddy_sensecraft_bridge.py [--dry-run]

Measurement map (channel "1"; standard IDs only — the Wio firmware and this
file must agree; see net_bridge.h). Do NOT add IDs outside this map without
verifying against the live platform: a trial "4109" beat on 2026-07-10
correlated with the OpenStream broker pushing nothing for either device
(uplinks still returned code 0). The develop-env broker was also found to
be backlogged (~25 min delivery lag) that day, so causation is unproven —
but stick to verified IDs; unknown IDs are documented to be dropped:
    4097 total          4102 session.pct      4106 today.tokens
    4098 running        4103 session.reset_s  4108 tz_offset_sec + 86400
    4099 waiting        4104 week.pct              (biased: the wire never
    4100 tokens         4105 week.reset_s           carries negatives)
    4101 tokens_today
"""
from __future__ import annotations

import base64
import json
import os
import sys
import time
import urllib.request
import uuid
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

CONFIG_FILE = Path(__file__).resolve().parent / "sensecraft_config.json"
DEFAULT_API_BASE = "https://intranet-sensecap-env-expose-publicdns.seeed.cc"
DEFAULT_INTERVAL_S = 10          # cloud uplink; device liveness window is 30 s
CHANNEL = "1"
# Channel declaration: measurements on an undeclared channel are silently
# dropped by the platform (uplinks still return code 0). The declaration can
# be LOST platform-side (observed 2026-07-10 on the develop env: overnight
# restart -> every measurement dropped until re-declared), so it is re-sent
# every RE_DECLARE_S per device, not just once per process.
# sensorType 1001 is arbitrary but valid.
SENSOR_TYPE = "1001"
RE_DECLARE_S = 600

TZ_BIAS = 86400                  # keeps the tz-offset measurement non-negative


def normalize_config(raw: dict, env: "dict[str, str]") -> dict:
    """Normalize file config + environment into {api_base, interval_s, devices}.

    One bridge process uplinks the SAME measurements to every configured
    device (e.g. a Wio Terminal and a SenseCAP Indicator side by side).
    Device precedence: SENSECRAFT_DEVICE_EUI/KEY env pair > raw["devices"]
    list > legacy raw["eui"]/raw["key"] scalars (still supported).
    """
    out = {
        "api_base": env.get("SENSECRAFT_API_BASE",
                            raw.get("api_base", DEFAULT_API_BASE)).rstrip("/"),
        "interval_s": int(env.get("BUDDY_INTERVAL_S",
                                  raw.get("interval_s", DEFAULT_INTERVAL_S))),
    }
    if env.get("SENSECRAFT_DEVICE_EUI") or env.get("SENSECRAFT_DEVICE_KEY"):
        devices = [{"eui": env.get("SENSECRAFT_DEVICE_EUI", ""),
                    "key": env.get("SENSECRAFT_DEVICE_KEY", "")}]
    elif raw.get("devices"):
        devices = [{"eui": d.get("eui", ""), "key": d.get("key", "")}
                   for d in raw["devices"]]
    else:
        devices = [{"eui": raw.get("eui", ""), "key": raw.get("key", "")}]
    if not devices or any(not d["eui"] or not d["key"] for d in devices):
        sys.exit(
            "missing device credentials. Provision a devkit and configure it:\n"
            "  sensecraft-cli device devkit create --sku blank_device --name claude-buddy\n"
            "  sensecraft-cli device devkit key --eui <EUI>\n"
            "then set SENSECRAFT_DEVICE_EUI/SENSECRAFT_DEVICE_KEY or copy\n"
            "sensecraft_config.example.json to sensecraft_config.json and fill it\n"
            'in ("devices": [{"eui": ..., "key": ...}, ...] uplinks to several\n'
            "devices from this one process).")
    out["devices"] = devices
    return out


def load_config() -> dict:
    raw = {}
    if CONFIG_FILE.exists():
        try:
            raw = json.loads(CONFIG_FILE.read_text())
        except ValueError as e:
            sys.exit(f"bad JSON in {CONFIG_FILE}: {e}")
    return normalize_config(raw, dict(os.environ))


def measurements(hb: dict, snap: dict, tz_off: int) -> "dict[str, str]":
    """Flatten heartbeat + v:1 snapshot into the wire measurement dict."""
    return {
        "4097": str(hb["total"]),
        "4098": str(hb["running"]),
        "4099": str(hb["waiting"]),
        "4100": str(hb["tokens"]),
        "4101": str(hb["tokens_today"]),
        "4102": str(snap["session"].get("pct", 0)),
        "4103": str(snap["session"].get("reset_s", 0)),
        "4104": str(snap["week"].get("pct", 0)),
        "4105": str(snap["week"].get("reset_s", 0)),
        "4106": str(snap["today"].get("tokens", 0)),
        "4108": str(tz_off + TZ_BIAS),
    }


def uplink_body(dev: dict, meas: "dict[str, str]", declare_channel: bool) -> dict:
    ms = str(int(time.time() * 1000))
    events = [{
        "name": "measure-sensor",
        "value": [{"channel": CHANNEL, "measurements": meas, "measureTime": ms}],
    }]
    if declare_channel:
        events.append({
            "name": "update-channel-info",
            "value": [{"channel": CHANNEL, "sensorType": SENSOR_TYPE, "status": "normal"}],
            "timestamp": ms,
        })
    return {
        "requestId": str(uuid.uuid4()),
        "timestamp": ms,
        "intent": "event",
        "deviceEui": dev["eui"],
        "deviceKey": dev["key"],
        "events": events,
    }


def post_uplink(api_base: str, dev: dict, body: dict) -> None:
    auth = base64.b64encode(f"{dev['eui']}:{dev['key']}".encode()).decode()
    req = urllib.request.Request(
        api_base + "/deviceapi/kit/message_uplink",
        data=json.dumps(body, separators=(",", ":")).encode("utf-8"),
        headers={"Authorization": f"Device {auth}",
                 "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=15) as r:
        d = json.loads(r.read())
    if str(d.get("code")) != "0":
        raise RuntimeError(f"uplink rejected: {d}")


def run(dry_run: bool) -> None:
    sys.stdout.reconfigure(line_buffering=True)   # keep logs live when redirected
    cfg = load_config()
    tz = datetime.now().astimezone().tzinfo
    tz_off = int(datetime.now(tz).utcoffset().total_seconds())
    creds = load_creds()
    caps = caps_for(creds["subscriptionType"], creds["rateLimitTier"])
    token, ua = load_token(), claude_code_ua()
    scanner = IncrementalScanner(PROJECTS)
    events: list = []
    probed = None
    last_probe = 0.0
    for dev in cfg["devices"]:       # per-device runtime state
        dev.update(declared_at=0.0,  # last update-channel-info; re-sent every
                   backoff=0,        # RE_DECLARE_S in case the platform loses
                   next_try=0.0)     # it. backoff doubles 5..60 s on failure.
    euis = ", ".join(d["eui"] for d in cfg["devices"])
    print(f"uplinking to {cfg['api_base']} as [{euis}] every {cfg['interval_s']}s")
    while True:
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

        hb = heartbeat(events, now)
        meas = measurements(hb, snap, tz_off)
        sent = []
        for dev in cfg["devices"]:
            if now < dev["next_try"]:          # this device is backing off
                continue
            declare = now - dev["declared_at"] >= RE_DECLARE_S
            body = uplink_body(dev, meas, declare)
            if dry_run:
                print(json.dumps(body, indent=2))
                continue
            try:
                post_uplink(cfg["api_base"], dev, body)
                if declare:
                    dev["declared_at"] = now
                dev["backoff"] = 0
                sent.append(dev["eui"])
            except Exception as e:
                dev["backoff"] = min(60, dev["backoff"] * 2) if dev["backoff"] else 5
                dev["next_try"] = now + dev["backoff"]
                print(f"uplink to {dev['eui']} failed ({e}); "
                      f"retrying in {dev['backoff']}s")
        if dry_run:
            return
        if sent:
            src = snap["session"].get("src", "est")
            print(f"sent to [{', '.join(sent)}] ({src}): "
                  f"session {snap['session']['pct']}%  "
                  f"week {snap['week']['pct']}%  running {hb['running']}")
        time.sleep(cfg["interval_s"])


if __name__ == "__main__":
    try:
        run("--dry-run" in sys.argv[1:])
    except KeyboardInterrupt:
        print("\nbye")
