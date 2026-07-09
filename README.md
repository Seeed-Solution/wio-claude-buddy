# Wio Claude Buddy

[![build](https://github.com/Rida2000/wio-claude-buddy/actions/workflows/build.yml/badge.svg)](https://github.com/Rida2000/wio-claude-buddy/actions/workflows/build.yml)

A desk companion for **Claude Code**, running on a **Seeed Wio Terminal**. An
animated ASCII pet reacts to your Claude activity, and the screen shows your live
**sessions**, a Tamagotchi-style **pet-stats** panel, and your real **plan-usage
gauges** (5‑hour session limit + weekly limit) — wirelessly, via the
**SenseCraft Data Platform** (WiFi + MQTT, the default) or over **Bluetooth**
(`-DBUDDY_BLE` build).

It's a Wio Terminal port of Anthropic's
[`claude-desktop-buddy`](https://github.com/Links17/claude-desktop-buddy)
reference firmware, plus a small Python bridge that feeds it data from your
`~/.claude` logs and Claude's usage API.

```
+------------------------------------------------------+
| Claude Buddy        [busy]        today 89000     o  |   name · state · tokens · link
+---------------------------+--------------------------+
|                           | USAGE                    |
|        /\_/\              |  session 5h        30%   |
|       ( o.o )             |  [#####.............]    |
|        > ^ <    busy      |  resets 1h38m            |
|                           |  weekly            43%   |
|     (animated pet)        |  [#########.........]    |
|                           |  resets Sat 21:00        |
+---------------------------+--------------------------+
| appr 0  deny 0  Lv 2                                 |
+------------------------------------------------------+
```

Press **B** to cycle the right pane between **SESSIONS**, **PET STATS**
(mood / fed / energy / level), and **USAGE**.

---

## What you need

**Hardware**
- A [Seeed Wio Terminal](https://www.seeedstudio.com/Wio-Terminal-p-4509.html)
- A USB‑C **data** cable (not charge‑only)
- *(optional)* a microSD card — only for custom GIF character packs

**Software** (host computer that runs Claude Code)
- [`arduino-cli`](https://arduino.github.io/arduino-cli/latest/installation/)
- Python 3.9+
- macOS, Linux, or Windows. The usage‑bridge's token lookup is written for
  **macOS** (reads the Keychain); see *Token on Linux/Windows* below.

---

## How it works

Two pieces:

1. **Firmware** (`firmware/claude_buddy/`) — runs on the Wio and renders the
   buddy + panels from JSON. The default build joins your WiFi and subscribes
   to the SenseCraft **OpenStream MQTT** broker; the `-DBUDDY_BLE` build is the
   original BLE Nordic UART peripheral.
2. **Host bridge** — runs on your computer, reads `~/.claude`, and produces:
   - a **session heartbeat** (drives the pet + sessions), and
   - your **plan usage** (session + weekly %), fetched from Claude's own usage
     endpoint for exact numbers.

   `buddy_sensecraft_bridge.py` (default) uplinks those as numeric measurements
   to the SenseCraft Data Platform, which pushes them to the Wio over MQTT.
   `buddy_ble_bridge.py` streams the same JSON directly over Bluetooth.

The RTL8720 radio runs **one transport per build** (rpcWiFi and rpcBLE can't
coexist), so pick a column:

| Data path | Pet + sessions | Recent-session lines | Live approve/deny prompts | Usage gauges | Cloud dashboard | Survives disconnects |
|--------|:---:|:---:|:---:|:---:|:---:|:---:|
| **SenseCraft** (default build + `buddy_sensecraft_bridge.py`) | ✅ | ❌ | ❌ | ✅ | ✅ | ✅ auto-reconnect |
| **BLE bridge** (`-DBUDDY_BLE` + `buddy_ble_bridge.py`) | ✅ | ✅ | ❌ | ✅ | ❌ | ❌ replug needed |
| Claude Desktop's *Hardware Buddy* (`-DBUDDY_BLE`) | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ replug needed |

SenseCraft carries **numbers only** — the transcript-entry strings can't ride a
telemetry platform, so the SESSIONS pane shows counts and a synthesized status
line there. Everything else (pet states, gauges, countdowns, clock) is
identical.

---

## Step 1 — Install the toolchain

```bash
# 1. arduino-cli  (https://arduino.github.io/arduino-cli/latest/installation/)
#    macOS:  brew install arduino-cli      Linux: curl ... | sh

# 2. Seeed Wio Terminal board package
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:samd

# 3. Firmware libraries
arduino-cli lib install "ArduinoJson" "AnimatedGIF" \
  "Grove-3-Axis-Digital-Accelerometer-2g-to-16g-LIS3DHTR" \
  "Seeed Arduino rpcWiFi" "PubSubClient" \
  "Seeed Arduino rpcBLE" "Seeed Arduino rpcUnified"
# (GIF character packs only) also:  "Seeed Arduino FS" "Seeed Arduino SFUD"
```

> The Wio's LCD driver (`Seeed_Arduino_LCD`, a TFT_eSPI fork) is **bundled in the
> board package** — no separate install. If you *also* have a generic `TFT_eSPI`
> installed, force the right one with `--library` (see Step 2).

---

## Step 2 — Provision SenseCraft + build & flash the firmware

**Provision a virtual device** (once) on the SenseCraft Data Platform with
[`sensecraft-cli`](https://www.npmjs.com/package/@seeed-studio/sensecraft-cli),
then create the firmware's credentials file:

```bash
npx @seeed-studio/sensecraft-cli auth init      # OAuth login (browser)
sensecraft-cli device devkit create --sku blank_device --name claude-buddy
#   -> {"eui": "...", "device_key": "..."}       (key again later: devkit key --eui <EUI>)

cd firmware/claude_buddy
cp secrets_example.h secrets.h                   # gitignored — fill in:
#   WIFI_SSID / WIFI_PASS      your 2.4 GHz network
#   MQTT_HOST                  OpenStream broker (prod: sensecap-openstream.seeed.cc)
#   SC_ORG_ID / SC_ACCESS_KEY  org ID + API access key (portal security page)
#   SC_DEVICE_EUI              the devkit EUI from above
```

The Wio's SAMD51 must be in its **bootloader** to flash reliably. **Enter it by
sliding the power switch (left side) down twice quickly** (a "double‑tap"); the
screen dims and it mounts for upload. Then:

```bash
# find your port (e.g. /dev/cu.usbmodemXXXX on macOS, /dev/ttyACM0 on Linux)
arduino-cli board list

# build + upload (default build: ASCII buddy + WiFi/SenseCraft + usage)
LCD=~/Library/Arduino15/packages/Seeeduino/hardware/samd/1.8.5/libraries/Seeed_Arduino_LCD
arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" \
  -u -p /dev/cu.usbmodemXXXX claude_buddy.ino

# BLE build instead (Claude Desktop Hardware Buddy, or the BLE bridge):
arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" \
  --build-property "compiler.cpp.extra_flags=-DBUDDY_BLE" \
  -u -p /dev/cu.usbmodemXXXX claude_buddy.ino
```

After it flashes, **unplug and replug the USB cable once** to run the firmware.
You should see a brief "Claude Buddy" splash, then the dashboard (the link dot
top‑right is red until the bridge connects).

> **macOS LCD path:** the `$LCD` path above pins core version `1.8.5`; adjust if
> `arduino-cli core list` shows a different version. On Linux the path is under
> `~/.arduino15/packages/...`. The `--library` override is only needed if a
> conflicting `TFT_eSPI` is installed — otherwise you can omit it.

---

## Step 3 — Run the host bridge

**SenseCraft (default).** Stdlib-only — no pip install needed:

```bash
cd host
cp sensecraft_config.example.json sensecraft_config.json   # fill in eui + key
python3 buddy_sensecraft_bridge.py          # or configure via env vars:
# SENSECRAFT_API_BASE / SENSECRAFT_DEVICE_EUI / SENSECRAFT_DEVICE_KEY / BUDDY_INTERVAL_S
```

```
uplinking to https://sensecap.seeed.cc as 1CF7... every 10s
sent (exact): session 30%  week 43%  running 1
```

The Wio (on WiFi, subscribed to OpenStream) picks each uplink up within a
second or two. Try `python3 buddy_sensecraft_bridge.py --dry-run` to print one
uplink body without sending, and
`sensecraft-cli data latest --eui <EUI>` to confirm the platform is storing.

**BLE build instead:**

```bash
cd host
python3 -m venv .venv
. .venv/bin/activate            # Windows: .venv\Scripts\activate
pip install -r requirements.txt # installs bleak

python buddy_ble_bridge.py
```

With the Wio powered on and showing the dashboard (and **not** paired in Claude
Desktop), the bridge finds `Claude Wio`, connects, and starts streaming. You'll
see:

```
connected to Claude Wio
sent (exact): session 30%  week 43%  running 1
```

On the Wio, press **B** to reach the **USAGE** pane — it now shows your real
session and weekly percentages with live reset countdowns.

---

## Step 4 — Using it

| Input | When idle | When a prompt is pending* |
|-------|-----------|---------------------------|
| **A** (right top button) | cycle pet species | approve once* |
| **B** (middle top button) | cycle right pane (sessions / pet stats / usage) | — |
| **C** (left top button) | — | deny* |
| joystick up / down | scroll the transcript | — |
| shake the device | dizzy animation | dizzy animation |
| lay it face‑down | nap (refills the energy bar) | — |

\* Approval prompts only appear when driven by Claude Desktop's Hardware Buddy
(a `-DBUDDY_BLE` build) — the bridges derive data from `~/.claude`, which has no
pending‑prompt info, and the SenseCraft downlink carries telemetry only.

The **pet's mood** comes from how fast you approve and your approve/deny ratio;
**fed** and **level** come from tokens used; **energy** drains over time and
refills when you set the device face‑down. (These are kept in RAM and reset on
reboot — see *Why no persistence* in Troubleshooting.)

---

## Configuration

**SenseCraft bridge** — env vars or `host/sensecraft_config.json` (gitignored):

| Setting | Default | Meaning |
|----------|---------|---------|
| `SENSECRAFT_API_BASE` / `api_base` | Seeed develop env | platform base URL (international prod: `https://sensecap.seeed.cc`) |
| `SENSECRAFT_DEVICE_EUI` / `eui` | — | blank_device devkit EUI |
| `SENSECRAFT_DEVICE_KEY` / `key` | — | devkit device_key |
| `BUDDY_INTERVAL_S` / `interval_s` | `10` | uplink period (device sleeps after 30 s of silence) |

**BLE bridge** — constants at the top of `host/buddy_ble_bridge.py`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `INTERVAL_S` | `5` | how often the pet/session heartbeat is sent (local, cheap) |

Both bridges share `USAGE_POLL_S = 60` (`host/buddy_common.py`) — how often the
**exact** usage is fetched from Claude's API. Keep it at ~60 s or higher — the
endpoint is rate‑limited. The on‑screen reset countdown ticks every second
between fetches, so it never looks frozen.

**Measurement map** (SenseCraft path). The platform silently drops custom
measurement IDs on a blank_device, so standard IDs are repurposed — the host
flatten (`buddy_sensecraft_bridge.py`) and the firmware parser (`net_bridge.h`)
must agree. All on channel 1:

| ID | Field | ID | Field |
|----|-------|----|-------|
| 4097 | sessions total | 4103 | session reset (s) |
| 4098 | sessions running | 4104 | week % |
| 4099 | sessions waiting | 4105 | week reset (s) |
| 4100 | tokens | 4106 | today tokens |
| 4101 | tokens today | 4108 | tz offset (s) + 86400 |
| 4102 | session % | | |

(The tz offset is biased by +86400 so the wire never carries a negative; the
firmware subtracts it back and sets its soft RTC from the measurement
timestamps — no NTP.) Side effect: SenseCraft dashboards label these with their
standard sensor names ("Air Temperature" = total sessions, etc.).

**Custom GIF characters** (instead of ASCII pets): build with `-DBUDDY_GIF`,
insert a FAT‑formatted microSD, and put a pack at `/characters/<name>/`
(`manifest.json` + 96px GIFs; a sample is in `characters/bufo/`). This pulls in
`Seeed Arduino FS`/`SFUD`. It's off by default because that filesystem library's
global initializer was crashing on cold boot (see Troubleshooting).

```bash
arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" \
  --build-property "compiler.cpp.extra_flags=-DBUDDY_GIF" -u -p <port> claude_buddy.ino
```

---

## Where the usage numbers come from

The bridge calls **`GET https://api.anthropic.com/api/oauth/usage`** — the same
endpoint Claude Code itself uses — which returns exact `five_hour` and `seven_day`
utilization (%) and reset timestamps.

It authenticates with **your own** OAuth token. On macOS that token is read from
the **Keychain** (`security find-generic-password -s "Claude Code-credentials"`),
because the copy in `~/.claude/.credentials.json` is usually expired. The token
goes only to `api.anthropic.com` (Anthropic's official host). If the fetch fails
or is rate‑limited, the bridge falls back to a local estimate from `~/.claude`.

This endpoint is **undocumented** and rate‑limited — Anthropic could change it
without notice. It's used here purely to mirror your own usage on your own
display.

**Token on Linux/Windows:** Claude Code may store the token differently there
(file vs. libsecret vs. DPAPI). Replace `_oauth()` in `buddy_ble_bridge.py` with
however your platform exposes a *current* `claudeAiOauth.accessToken`. The file
fallback (`~/.claude/.credentials.json`) works if it isn't expired.

---

## Troubleshooting

These were all real failures during bring‑up — fixes are already in the code.

- **Flashing fails / "uploading error: exit status 1".** The auto‑reset can't
  reach the bootloader if the app is busy. **Double‑tap the power switch** to
  enter the bootloader, then upload. After a from‑bootloader flash, **replug** to
  run the app.
- **White screen.** A 16‑bit full‑screen sprite (150 KB) doesn't fit beside the
  BLE stack's RAM; this firmware uses an 8‑bit sprite (77 KB) so it fits. If you
  see a red "sprite alloc failed" screen, you're out of RAM.
- **Dark screen / blue LED blinking after a reboot.** A hard fault. The known
  cause was the `Seeed_FS`/`SD` library's global initializer faulting on cold
  boot — that's why GIF/filesystem support is the opt‑in `-DBUDDY_GIF` build and
  the default build omits it.
- **Board crashes when you press a button.** Was a flash (NVM) write blocking the
  BLE stack. Fixed: persistence is now RAM‑only (*Why no persistence* below).
- **(NET build) buddy stays "No Claude connected".** Watch the serial log:
  `[net] wifi joining` → `wifi ok` → `mqtt subscribed`. WiFi must be 2.4 GHz
  (the RTL8720's 5 GHz support is spotty); `mqtt failed rc=4/5` means wrong
  org ID / access key. Then check the host bridge is uplinking and
  `sensecraft-cli data latest --eui <EUI>` shows fresh points. Unlike BLE,
  the NET build **reconnects by itself** after WiFi or broker drops — no
  replugging.
- **(BLE build) bridge says "not found".** The Wio isn't advertising — boot it
  (replug) and make sure it isn't paired in Claude Desktop.
- **(BLE build) bridge can't reconnect after you restart it.** The firmware
  intentionally **doesn't re‑advertise after a disconnect** (re‑advertising was
  crashing rpcBLE). **Replug the Wio**, then start the bridge.
- **Usage shows 100% / wrong.** The usage probe failed (e.g. expired token →
  401) and it fell back to the local estimate, which over‑counts cache tokens.
  Confirm the Keychain has a current token.

**Why no persistence:** writing stats/species to flash (`FlashStorage`) blocks
for milliseconds and wedges the rpcBLE link, so the device keeps stats in RAM
only — they reset on reboot. Acceptable trade for stability.

---

## Project layout

```
firmware/
  claude_buddy/         the buddy firmware (Arduino sketch)
    claude_buddy.ino    setup/loop, split-view UI, state machine
    wio_platform.*      Wio hardware shim (display, buttons, accel, buzzer, soft RTC)
    transport.h         tr* dispatch: NET (default) / -DBUDDY_BLE / MOCK stubs
    net_bridge.*        rpcWiFi + MQTT (SenseCraft OpenStream) transport
    ble_bridge.*        rpcBLE Nordic UART peripheral (-DBUDDY_BLE)
    secrets_example.h   WiFi + SenseCraft credential template (copy to secrets.h)
    data.h              wire-protocol parsing (heartbeat + usage v:1)
    buddy*.{h,cpp}      ASCII pet engine
    buddy_sp_*.cpp      18 pet species
    character.*         GIF character support (opt-in -DBUDDY_GIF)
    stats.h             mood/fed/energy/level
    prefs_compat.h      RAM-only settings store
    b64.h, xfer.h       base64 + folder-push (GIF) receiver
    test/test_b64.cpp   native unit test
  ble_probe/            minimal BLE write-path diagnostic sketch
  wifi_probe/           staged WiFi+MQTT diagnostic sketch (on-screen stages)
  claude_buddy_indicator/  SenseCAP Indicator port (ESP32-S3, ESP-IDF v5.1) — WIP
    main/               app + WiFi/esp-mqtt shell + Arduino/platform shims
    components/buddy_core/       byte-identical copies of the Wio's shared headers
    components/net_bridge_core/  shared SenseCraft wire-format core
    components/sensecap_bsp/     Seeed's BSP (git submodule)
host/
  buddy_sensecraft_bridge.py  SenseCraft uplink bridge (default, stdlib-only)
  buddy_ble_bridge.py   the unified BLE bridge
  buddy_common.py       shared ~/.claude + usage-probe core
  sensecraft_config.example.json
  claude_meter/         ~/.claude parsing + usage math (vendored)
  requirements.txt      (BLE bridge only — bleak)
characters/bufo/        sample GIF character pack
```

---

## SenseCAP Indicator port (in progress)

`firmware/claude_buddy_indicator/` runs the same buddy data pipeline on a
[SenseCAP Indicator](https://wiki.seeedstudio.com/Develop_with_SenseCAP_Indicator/)
(ESP32-S3, 4" 480×480 touch). Phase 1 (done): the Wio's wire-format and
parsing headers compile **verbatim** under ESP-IDF v5.1, with a WiFi +
esp-mqtt shell subscribing to the same SenseCraft OpenStream topics, a boot
self-test, and heap/fragmentation logging for hardware bring-up. Phase 2
(next): LVGL pet UI and the on-device WiFi provisioning flow (scan → tap →
on-screen keyboard → remembered networks → disconnect/forget). Design and
plan: `docs/superpowers/specs/`, `docs/superpowers/plans/`.

The Indicator is its **own SenseCraft device**: provision a second devkit
(`sensecraft-cli device devkit create --sku blank_device --name
claude-buddy-indicator`), then add its EUI/key to the `"devices"` list in
`host/sensecraft_config.json` — one bridge process feeds the Wio and the
Indicator simultaneously.

```bash
git submodule update --init
. ~/esp-idf-v5.1/export.sh     # ESP-IDF v5.1.x only (BSP requirement)
cd firmware/claude_buddy_indicator
cp main/indicator_secrets_example.h main/indicator_secrets.h  # fill in
idf.py set-target esp32s3 && idf.py -p /dev/cu.usbmodem* flash monitor
```

Expected on the monitor: `SELF-TEST OK` (wire format verified without any
network), `[heap]` lines at boot / wifi-up / mqtt-up, then live
`total/running/session%` lines as the host bridge uplinks.

---

## Credits & license

Port of [`claude-desktop-buddy`](https://github.com/Links17/claude-desktop-buddy)
© Anthropic, PBC — **MIT** (see `LICENSE`). The 18 ASCII species, the Nordic UART
protocol, and the `bufo` sample pack come from upstream. The Wio platform port,
the landscape split‑view UI, the plan‑usage panel, and the BLE bridge are this
project's additions.

`host/claude_meter/` (the `~/.claude` parsing + usage math used by the bridge) is
vendored from the author's own *claude-usage* meter, MIT (see
`host/claude_meter/NOTICE`).
