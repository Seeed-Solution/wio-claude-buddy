# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-part desk companion for Claude Code running on a **Seeed Wio Terminal**
(SAMD51, 320×240 LCD): an Arduino **firmware** that renders an animated ASCII pet
plus session/usage panels, and a Python **host bridge** that feeds it data from
`~/.claude` and Claude's usage API. The default transport is **WiFi + SenseCraft
Data Platform** (host POSTs measurements; Wio subscribes to OpenStream MQTT);
the original **BLE** link remains behind `-DBUDDY_BLE`. It's a Wio port of
[`claude-desktop-buddy`](https://github.com/Links17/claude-desktop-buddy).

`README.md` is the source of truth for hardware setup, wiring, and the full
troubleshooting log — read it for anything physical-device related.

## Commands

### Firmware (`firmware/claude_buddy/`)

```bash
# Default build (ASCII buddy + WiFi/SenseCraft MQTT + usage). On macOS pin the
# bundled Wio LCD lib. Needs libs: "Seeed Arduino rpcWiFi" "PubSubClient".
# Credentials come from secrets.h (gitignored; template secrets_example.h).
LCD=~/Library/Arduino15/packages/Seeeduino/hardware/samd/1.8.5/libraries/Seeed_Arduino_LCD
arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" claude_buddy.ino

# BLE build (Claude Desktop Hardware Buddy / buddy_ble_bridge.py):
arduino-cli compile ... --build-property "compiler.cpp.extra_flags=-DBUDDY_BLE" claude_buddy.ino

# Build + flash to a board (device must be in bootloader: double-tap power switch):
arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" \
  -u -p /dev/cu.usbmodemXXXX claude_buddy.ino   # then UNPLUG/REPLUG to run

# Emulator build (canned demo data, no BLE — for the web SAMD emulator):
arduino-cli compile ... --build-property "compiler.cpp.extra_flags=-DMOCK_DATA" claude_buddy.ino

# GIF character packs (opt-in; pulls in Seeed_FS/SFUD, needs a microSD):
arduino-cli compile ... --build-property "compiler.cpp.extra_flags=-DBUDDY_GIF" ... claude_buddy.ino

# Native unit test for the base64 decoder (no board needed):
g++ -std=c++17 firmware/claude_buddy/test/test_b64.cpp -o /tmp/test_b64 && /tmp/test_b64
```

The `--library "$LCD"` override is only needed when a conflicting generic
`TFT_eSPI` is installed; otherwise omit it (CI does). The LCD driver ships inside
the board package.

### Indicator firmware (`firmware/claude_buddy_indicator/`, ESP-IDF)

SenseCAP Indicator port (ESP32-S3, 480×480 touch). Design + plan live in
`docs/superpowers/specs/` / `docs/superpowers/plans/`. **ESP-IDF v5.1.x only**
(the SenseCAP_Indicator_ESP32 BSP's pinned requirement; the BSP is the
`components/sensecap_bsp` submodule).

```bash
git submodule update --init                       # BSP (once)
. ~/esp-idf-v5.1/export.sh                        # or wherever v5.1.x lives
cd firmware/claude_buddy_indicator
idf.py set-target esp32s3 && idf.py build         # what CI runs
idf.py -p /dev/cu.usbmodem* flash monitor         # device flashes over USB-C
# Credentials: cp main/indicator_secrets_example.h main/indicator_secrets.h
# (gitignored). The Indicator is its OWN sensecraft-cli devkit — new EUI.

# Native wire-format test (no board):
g++ -std=c++17 firmware/claude_buddy/test/test_net_bridge_core.cpp -o /tmp/t && /tmp/t
```

**Shared-file rule:** `data.h`, `xfer.h`, `b64.h`, `stats.h`, `prefs_compat.h`
(in `components/buddy_core/`) and `net_bridge_core.h` are byte-identical
copies of the Wio originals — `scripts/check_shared_files.sh` fails CI on
drift; edit both copies in one commit. They compile verbatim because
`main/shim/` supplies `Arduino.h`/`wio_platform.h`/`transport.h` via
include-path precedence (never edit a shared header to "fix" an include).
`net_bridge_core.h` holds static state: exactly ONE translation unit per
program includes it (Wio: `net_bridge.cpp`; Indicator: `net_bridge_shell.c`;
tests: `test_net_bridge_core.cpp`). Use template functions, not macro
min/max, in shims — macros break libstdc++ under gnu++2b.

### Host bridge (`host/`)

```bash
cd host
# SenseCraft bridge (default path, stdlib-only, no venv needed):
#   config: env vars or sensecraft_config.json (see sensecraft_config.example.json)
#   a "devices" list uplinks to several devices (Wio + Indicator) from ONE
#   process — per-device declaration + backoff; legacy eui/key still works
python3 buddy_sensecraft_bridge.py               # POSTs measurements every 10 s
python3 buddy_sensecraft_bridge.py --dry-run     # print one uplink body per device
python3 -m unittest test_sensecraft_config       # config normalization tests

# BLE bridge (for -DBUDDY_BLE firmware):
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt        # only dep is bleak; claude_meter is stdlib-only
python buddy_ble_bridge.py             # finds "Claude Wio" over BLE and streams

# Lint (what CI runs):
python -m py_compile host/buddy_ble_bridge.py host/buddy_common.py \
  host/buddy_sensecraft_bridge.py host/claude_meter/*.py
```

Devkit provisioning (once, `sensecraft-cli` = `@seeed-studio/sensecraft-cli` npm):
`sensecraft-cli device devkit create --sku blank_device --name claude-buddy`,
key via `... devkit key --eui <EUI>`, verify data with `... data latest --eui <EUI>`.

CI (`.github/workflows/build.yml`) on every push/PR: compiles the NET (default),
`-DBUDDY_BLE`, and `-DMOCK_DATA` firmware variants, runs the native base64 test,
and byte-compiles the host. There is no pytest/ruff config — keep the host
import-clean and byte-compilable.

## Architecture

### One radio transport per build

The RTL8720 coprocessor runs **one** of rpcWiFi/rpcBLE per build, dispatched
through `transport.h` (`tr*` functions): default = `net_bridge` (WiFi +
SenseCraft OpenStream MQTT), `-DBUDDY_BLE` = `ble_bridge` (NUS peripheral),
`-DMOCK_DATA` = inline no-op stubs. Under BLE you pick *either*
`buddy_ble_bridge.py` (pet + sessions + real usage gauges) *or* Claude Desktop's
built-in Hardware Buddy (pet + live approve/deny prompts, no usage gauges). The
bridges derive everything from `~/.claude`, which has no pending-prompt info,
and the SenseCraft downlink is telemetry-only — the approval footer only lights
up under Desktop (BLE build).

### SenseCraft path (default)

`buddy_sensecraft_bridge.py` flattens the heartbeat + v:1 snapshot into numeric
measurements and POSTs `<api_base>/deviceapi/kit/message_uplink` (auth header
`Device base64(EUI:device_key)`, one-time `update-channel-info` declaration —
measurements on an undeclared channel are dropped) every 10 s. The platform
pushes each measurement to subscribers of
`/device_sensor_data/<org>/<eui>/<ch>/<rsvd>/<measID>` on the OpenStream broker
(payload `{"value":N,"timestamp":<ms>}`; MQTT :1883, user `org-<org>`, password
= org API access key). `net_bridge.cpp` accumulates them (500 ms quiet / 2.5 s
max debounce per group) and **synthesizes the original JSON lines** into the
same RX ring buffer BLE used — `data.h` is transport-agnostic and unchanged.
Soft RTC syncs from measurement timestamps + the biased tz measurement (no NTP).

Measurement map (channel 1; **custom IDs are silently dropped** by the platform,
so standard IDs are reused — keep `buddy_sensecraft_bridge.py::measurements` and
`net_bridge.cpp::measIndex` in agreement): 4097 total, 4098 running, 4099
waiting, 4100 tokens, 4101 tokens_today, 4102 session.pct, 4103 session.reset_s,
4104 week.pct, 4105 week.reset_s, 4106 today.tokens, 4108 tz_offset_sec+86400.
Not transmitted (numeric-only wire): heartbeat `msg` (synthesized on-device),
`entries[]` (SESSIONS pane shows counts only), `week.reset_label` (countdown
fallback). NET builds have no device→host uplink: `trWrite` is a no-op, so
permission acks / GIF folder-push can't be driven (BLE-build features).

Unlike rpcBLE, the NET state machine **reconnects automatically** after WiFi or
broker drops — the "replug after disconnect" rule below is BLE-only.

### Wire protocol (newline-delimited JSON, NUS)

BLE is a Nordic UART Service (`6e400001-…`); RX write char is
`6e400002-…`. The host sends `\n`-terminated JSON, chunked to ≤160 bytes per
write (`send_line` in the bridge); the firmware reassembles by newline in
`data.h::_LineBuf` and dispatches in `_applyJson`. Three message shapes, all on
the same channel:

1. **Heartbeat** `{total, running, waiting, msg, entries, tokens, tokens_today}`
   — drives the pet state machine + SESSIONS pane. Built by `heartbeat()`.
2. **Usage snapshot** `{v:1, session:{pct,reset_s}, week:{pct,reset_s,reset_label}, today:{tokens}, …}`
   — drives the USAGE gauges. Built by `claude_meter.snapshot.build_snapshot`.
   `reset_s` counts down **locally at 1 Hz** on-device between pushes
   (`dataUsageTick`) so it never looks frozen.
3. **Time sync** `{time:[epoch_sec, tz_offset_sec]}` — sets the soft RTC once.

The same JSON path also handles permission prompts (`prompt:{id,tool,hint}`) and
folder-push/GIF commands (`xferCommand`, via `b64.h`/`xfer.h`) when driven by a
source that sends them.

### Host data pipeline (`host/claude_meter/`, vendored, stdlib-only)

`scanner.IncrementalScanner` tails `~/.claude/projects/**/*.jsonl` by byte offset
→ `UsageEvent` stream → `snapshot.build_snapshot` produces the v:1 dict, combining
a **local estimate** (`aggregator` + `pricing` caps) with an optional **exact
server probe**. The bridge fetches exact numbers from the undocumented
`GET https://api.anthropic.com/api/oauth/usage` endpoint (`probe_usage`) every
`USAGE_POLL_S` (60 s) and overrides the estimate; on failure (401/rate-limit) it
falls back to the estimate, which over-counts cache tokens. `claude_meter` is
vendored from the author's *claude-usage* meter — keep it self-contained; don't
add third-party deps.

**OAuth token source matters:** `_oauth()` reads the *current* token from the
macOS **Keychain** (`security find-generic-password -s "Claude Code-credentials"`)
because `~/.claude/.credentials.json` is usually expired. Porting to
Linux/Windows means replacing `_oauth()`.

### Firmware structure (`firmware/claude_buddy/`)

- `claude_buddy.ino` — `setup`/`loop`, the `PersonaState` machine
  (sleep/idle/busy/attention/celebrate/dizzy/heart, derived in `derive()`), the
  landscape split view (left = buddy, right = one of 3 panes cycled by button B),
  and button/accelerometer handling.
- `data.h` — the wire-protocol parser **and** the three runtime modes checked in
  priority order: `demo` (auto-cycling fakes), `live` (JSON seen in last ~10 s),
  `asleep` (zeros).
- `wio_platform.*` — hardware shim: display/sprite, buttons, accelerometer
  (shake/face-down), buzzer, and a **soft RTC** (M5-compatible structs so `data.h`
  ports unchanged).
- `transport.h` — the `tr*` dispatch header (declaration-only, safe next to
  TFT_eSPI). `net_bridge.*` — rpcWiFi + PubSubClient transport (default).
  `ble_bridge.*` — rpcBLE NUS peripheral (`-DBUDDY_BLE`).
  `secrets_example.h` → copy to gitignored `secrets.h` for real credentials.
  `buddy*.{h,cpp}` + `buddy_sp_*.cpp` — the ASCII pet engine; each of the 18
  species is one file exposing 7 state functions in `PersonaState` order
  (`Species::states[7]`).
- `character.*` + `xfer.h`/`b64.h` — GIF character packs, opt-in `-DBUDDY_GIF`.
- `stats.h` (mood/fed/energy/level) + `prefs_compat.h` (settings store).

## Non-obvious constraints (these are deliberate; don't "fix" them)

- **Persistence is RAM-only.** Writing stats/species to flash (`FlashStorage`)
  blocks for ms and wedges the rpcBLE link, so stats reset on reboot.
  `prefs_compat.h` is a RAM store, not real NVS.
- **8-bit sprite, not 16-bit.** A full-screen 16-bit sprite (≈150 KB) won't fit
  beside the BLE stack's RAM; the firmware uses an 8-bit sprite (≈77 KB). A red
  "sprite alloc failed" screen means you're out of RAM.
- **No re-advertise after disconnect (BLE builds).** Re-advertising crashed
  rpcBLE, so after a BLE drop you must **replug the Wio** before the bridge can
  reconnect. (NET builds reconnect automatically.)
- **GIF/filesystem is opt-in** because `Seeed_FS`/`SD`'s global initializer faults
  on cold boot. The default build omits it (`wio_platform.h` gates `BUDDY_FS`).
- **`MOCK_DATA` builds** stub out all `ble*` functions in `claude_buddy.ino` and
  force demo mode — the emulator has no filesystem and can't run the GIF decoder,
  so it always shows the ASCII buddy and injects a synthetic permission prompt on
  a timer to exercise the approval footer.

## Build/debug gotchas (learned the hard way on real hardware)

- **Flashing needs a MANUAL bootloader entry before EVERY upload.** Ask the user
  to double-tap the power switch (down twice quickly), **wait for them to
  confirm**, then upload — the 1200-baud auto-reset does *not* work when the app
  is hung/crashed (which is common while iterating). After a from-bootloader
  flash the user must **replug USB** to run the app (it doesn't auto-start). The
  port differs between bootloader and app modes — always auto-detect
  (`ls /dev/cu.usbmodem*`).
- **TFT_eSPI and rpcBLE/rpcWiFi cannot be in the same translation unit.**
  Arduino's `min()`/`max()` macros collide with the rpc STL headers (`error:
  macro "min" passed 3 arguments`). Keep radio code in its own `.cpp`
  (`ble_bridge.cpp`, `net_bridge.cpp`) and never `#include` an rpc header from
  the same file as `TFT_eSPI.h`. (`net_bridge.cpp` also avoids ArduinoJson —
  the downlink payload is parsed with `strstr`/`strtod`.)
- **`#include <time.h>` explicitly** where you use `gmtime_r`/`struct tm`
  (`data.h`) — the Seeed SAMD core doesn't pull it in via `Arduino.h`.
- **`firmware/ble_probe/`** is a minimal BLE write-path diagnostic (NUS + an
  on-screen write counter, no FS/sprite). Flash it to confirm the radio/receive
  path on hardware when the full firmware misbehaves.
- **Diagnosing a blank screen:** *white* (or red "sprite alloc failed") = OOM at
  `createSprite` (RAM too tight). *Dark + blinking LED* = a hard fault, usually a
  boot-time global constructor (this is how the `Seeed_FS` `SD` ctor crash
  showed). Serial across a reset is racy (USB re-enumerates), so prefer on-screen
  signals (e.g. temporary full-screen color stages) over serial when bisecting a
  boot crash.
- **Usage endpoint request details:** `GET api.anthropic.com/api/oauth/usage`
  needs headers `Authorization: Bearer <token>`, `User-Agent: claude-code/<ver>`
  (without the claude-code UA you hit an aggressively-429'd bucket), and
  `anthropic-beta: oauth-2025-04-20`. Response `utilization` is **0–100** (not
  0–1) and `resets_at` is an ISO timestamp → convert to a countdown. It's
  undocumented and rate-limited; poll ≤ once/min and keep the local-estimate
  fallback.

## Conventions

- Firmware is single-sprite, single-threaded Arduino C++; keep RAM tight and
  avoid blocking calls in `loop()` (they break BLE). Match the existing terse
  `snprintf`-into-fixed-buffer style.
- Host is Python 3.9+, stdlib + bleak only, with `from __future__ import
  annotations` and `"X | None"` string annotations for 3.9 compatibility.
- Upstream artifacts (18 species, NUS protocol, `bufo` pack) are ported as-is;
  the Wio platform shim, split-view UI, usage panel, and bridge are this repo's
  additions.
