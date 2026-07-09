# SenseCAP Indicator Migration — Design

Status: approved by user 2026-07-09 (touch-pet interaction, RP2040 out of scope,
5-network cap, new SenseCraft device via sensecraft-cli). Produced by a
research → 3-proposal brainstorm → adversarial judge panel → synthesis workflow.

## Decision

**Approach: ESP-IDF + LVGL, building on Seeed's official `SenseCAP_Indicator_ESP32`
BSP** (judge tally: ESP-IDF+LVGL 21, Arduino+LVGL 19, Arduino-minimal 15 — close,
so the strongest pieces of the losing proposals are grafted in rather than adopting
the winner unmodified).

Three reasons this wins:

1. **Touch/display bring-up is the single largest real risk** (ST7701 RGB panel +
   FT5x06 touch routed through a PCA9535 IO expander), and only this approach
   converts it from "hand-roll TouchLib against an undocumented PCA9535-routed
   INT/RESET" into "run the vendor's own validated example first."
2. **esp-mqtt over PubSubClient** for a subscribe-mostly client: background FreeRTOS
   task, tunable buffers, real reconnect backoff — versus PubSubClient's documented
   hang-after-disconnect behavior.
3. **LVGL gives the two hard-required touch flows their proven widget pattern**
   (`lv_list` scan page; `lv_textarea` + `lv_keyboard` with focus/defocus binding)
   rather than a hand-rolled hit-test keyboard with zero prior art on this board.

Judge-flagged weaknesses, resolved by construction (not papered over):

- **Wire-format fidelity.** `net_bridge.cpp`'s measurement-index table,
  debounce/`Group` struct, and `strstr`/`strtod` parsing move into a single
  portable `.c` file with zero Arduino or ESP-IDF API calls (only
  `<stdint.h>`/`<string.h>`/`<stdio.h>`). Both the Wio Arduino build and the
  Indicator IDF build compile this same file unmodified; only the thin outer shell
  (PubSubClient callback vs. esp-mqtt event handler) differs per platform — the
  same narrow-core-behind-adapter seam the buddy engine already uses.
- **CI diff-guard covers every shared file** (`data.h`, `stats.h`, `buddy.cpp`, all
  18 species files, the portable net-bridge core), not just one: CI fails if any
  copy diverges from `firmware/claude_buddy/` without a reviewed update to both.
- **`stats.h` stays byte-identical**: the NVS-backed preferences header is selected
  via include-path precedence (the Indicator component's `include/` dir is searched
  before the shared one), never by editing `stats.h`.
- **ESP-IDF/LVGL versions are pinned to whatever Seeed's vendored `bsp_board`
  component was built against** (community evidence points to LVGL 8.3.x), not
  floated to latest — the exact versions are confirmed in spike 1, not assumed.
- **No Arduino compatibility layer — shim headers instead.** `data.h` directly
  includes `<Arduino.h>`, `<ArduinoJson.h>`, `xfer.h`, and `wio_platform.h`, so
  keeping it byte-identical means the Indicator component ships its own headers
  under those exact names via the same include-path-precedence trick as
  `stats.h`: a minimal `Arduino.h` shim (`millis()` →
  `esp_timer_get_time()/1000` plus the few types `data.h` touches), a
  `wio_platform.h` that forwards to `indicator_platform.h` (soft-RTC structs,
  same signatures), an `xfer.h` stub (GIF transfer is deferred), and ArduinoJson
  vendored as an IDF component (header-only, no Arduino dependency in its core
  deserializer). The full set compiles standalone in spike 5.
- **esp-mqtt fragmentation.** A payload-reassembly buffer keyed by topic sits
  between the esp-mqtt event callback and the portable parsing core, so the
  `strstr` parser never sees a partial JSON value regardless of how esp-mqtt
  chunks delivery. (Exact event-fragmentation field names verified in spike 3.)

## Target architecture

**Toolchain**: ESP-IDF pinned to `bsp_board`'s tested release (verify in spike 1),
target `esp32s3`, `idf.py build/flash/monitor`. PSRAM (OPI mode) is mandatory —
for the 480×480 framebuffers and MQTT/LVGL heap headroom.

Directory layout (sibling to the existing Wio tree; nothing existing moves):

```
firmware/
  claude_buddy/            # unchanged — Wio Arduino sketch
  claude_buddy_indicator/  # new — ESP-IDF project
    CMakeLists.txt
    sdkconfig.defaults
    main/
      app_main.c               # boot FSM, LVGL init, screen wiring
      indicator_platform.c/.h  # display/touch/soft-RTC/lifecycle shim
      net_bridge_shell.c       # esp-mqtt glue around the portable core
      wifi_provision.c/.h      # scan / keyboard / NVS / disconnect UI
      nvs_prefs.c/.h           # Preferences-shaped NVS backend for stats.h
    components/
      sensecap_bsp/            # git submodule, pinned commit — vendor bsp_board
      buddy_core/              # data.h, stats.h, buddy.cpp, 18 species (diff-guarded)
      net_bridge_core/         # portable parse/debounce/JSON-synthesis .c (shared)
scripts/
  check_shared_files.sh        # CI diff-guard: fails on drift in shared files
```

```
                 ┌─────────────────────────────┐
                 │        app_main.c (FSM)      │
                 └───────────┬─────────────────┘
        ┌────────────────────┼─────────────────────┐
        ▼                    ▼                      ▼
 indicator_platform    wifi_provision          net_bridge_shell
 (LVGL/touch/RTC)      (scan/kbd/NVS)          (esp-mqtt events)
        │                    │                      │
        │                    ▼                      ▼
        │             nvs_prefs (Preferences)  net_bridge_core (portable .c)
        │                                            │  reassembly buffer
        ▼                                            ▼
   buddy_core (data.h, stats.h,               data.h::_applyJson
   buddy.cpp, 18 species — diff-guarded             │
   against firmware/claude_buddy/)            RX ring buffer (unchanged)
```

## Display & interaction

480×480 capacitive touch, driven by the vendored `bsp_board` LVGL port
(full-refresh mode for v1; direct-mode/tearing is a v2 concern). The buddy
engine's 4-primitive facade (`buddyPrintLine/Sprite`, `buddySetCursor/Color`,
`buddyPrint`, `setTextSize`) is reimplemented against an **8-bit indexed
`lv_canvas`** (not RGB565) — the same deliberate "8-bit sprite, not 16-bit" RAM
discipline the Wio build uses — keeping the pet's per-tick redraw one cheap blit
plus one `lv_obj_invalidate()`, not per-glyph widget churn. Monospace bitmap font
at roughly 2× the Wio's 6×8 cell for arm's-length legibility on a 4" panel; exact
size confirmed by spike 4.

**Layout**: `lv_tabview` with a persistent status bar (WiFi icon + connection
state + settings gear). Tabs: Pet (full canvas), Sessions, Pet-Stats, Usage —
swipe or tap replaces `btnBPressed()`'s pane cycle. Approve/deny become two
`lv_btn`s pinned to the Pet screen footer, hidden unless a prompt is pending
(dead path on NET builds today; kept for future parity).

**Touch-the-pet interactions** (user decision — replaces the Wio's
accelerometer gestures; there is no confirmed IMU on the Indicator):

- **Single tap on the pet canvas** = petting/affection → heart reaction +
  mood/fed stats bump (the touch equivalent of the Wio's pet-interaction button).
- **Rapid scrubbing** (≥4 taps or drag reversals within ~2 s on the canvas) =
  dizzy state — the shake replacement.
- **Sleep/nap** loses its face-down trigger and becomes idle-timer-only
  (`derive()` already has the timer path).
- Exact parity with the Wio's button-A/C handlers is finalized in milestone M2
  against `claude_buddy.ino`'s handler table; the mapping above is the contract.

## WiFi provisioning

```
BOOT
  └─▶ NVS_LOAD (read ≤5 saved profiles)
        └─▶ has entries?
              yes → TRY_LAST_OK (most-recently-successful profile, 8 s timeout)
                      success → RUN
                      fail → TRY_REMAINING (known profiles, RSSI order, 8 s each)
                                success → RUN
                                fail → SCAN_PAGE
              no  → SCAN_PAGE
RUN ──(gear icon, any time)──▶ SCAN_PAGE   (manual re-provision / add network)
```

**Scan page**: `lv_list` populated from `esp_wifi_scan_start(block=false)` +
`WIFI_EVENT_SCAN_DONE`, sorted by RSSI, deduped by SSID, lock glyph for secured
networks, checkmark for already-known SSIDs. Tap → password modal: `lv_textarea`
in password mode + `lv_keyboard` via LVGL's focus/defocus pattern; open networks
skip the modal. `LV_EVENT_READY` triggers `esp_wifi_connect()` off the LVGL task
(dedicated FreeRTOS task, event queue back to the UI) with a spinner and 15 s
timeout; success writes NVS, failure shows an inline error and stays on the modal.

**Disconnect and Forget are two distinct, always-visible actions**: the
status-bar detail view has a standalone **Disconnect** button
(`esp_wifi_disconnect()`, session-only, credential kept) separate from each
known-network row's **Forget** (trash icon → confirm → NVS entry removed, plus
disconnect if it was the active network).

**NVS schema**: namespace `wifi_creds`, key `count` (uint8, cap **5** — user
confirmed), fixed-size blobs `net{N}_ssid` (≤32 B), `net{N}_pass` (≤64 B),
`net{N}_last_ok` (int64 epoch). Writes happen only after a screen transition
confirms success, from a low-priority task, never from an LVGL callback (NVS
commits block; measured in spike 6).

**Rescan guard**: rescanning while RUN-state MQTT traffic is flowing requires an
explicit "scanning may briefly interrupt your connection" confirmation — an ESP32
scan forces off-channel time and disrupts in-flight STA traffic.

## SenseCraft data loop

**The Indicator is a NEW SenseCraft device** (user decision), provisioned once
with `sensecraft-cli` exactly like the Wio was:

```
sensecraft-cli device devkit create --sku blank_device --name claude-buddy-indicator
sensecraft-cli device devkit key --eui <NEW_EUI>
sensecraft-cli device data latest --eui <NEW_EUI>     # verify during bring-up
```

Same org, same OpenStream broker and org API key; only the EUI (and its device
key, used by the host for uplink auth) is new. The Indicator subscribes to
`/device_sensor_data/<org>/<NEW_EUI>/<ch>/<rsvd>/<measID>`.

**One host bridge feeds both devices simultaneously** (user decision — no
separate host version, no second process): `buddy_sensecraft_bridge.py` gains a
`devices: [{eui, key}, ...]` list in `sensecraft_config.json` (env-var and
legacy single `eui`/`key` config keep working as a one-device list) and POSTs
the same measurement batch to every device each cycle, tracking the one-time
`update-channel-info` declaration and failure backoff per device. The Wio and
the Indicator run side by side off a single bridge process.

Device-side credentials (org ID, org API key, EUI) live in NVS alongside the
WiFi profiles — no compile-time `secrets.h` on this target. Initial provisioning:
a gitignored CSV → `nvs_partition_gen.py` image flashed with the firmware (the
NVS analog of `secrets.h`); an on-screen credential editor is deferred.

MQTT client: **esp-mqtt** (`esp_mqtt_client`), port 1883, background task,
built-in reconnect backoff. `net_bridge_core` carries the measurement-ID map
(4097–4108, channel 1), the 500 ms/2.5 s debounce `Group` struct, and
`flushHeartbeat/flushUsage/flushTimeSync` JSON synthesis **unmodified from the
Wio original**, wrapped by the topic-keyed reassembly buffer. `data.h`'s
`_applyJson`/`_LineBuf`/mode-priority logic is untouched — it still consumes only
`trAvailable()/trRead()` and the same `RTC_TimeTypeDef`/`RTC_DateTypeDef`
soft-RTC structs, reimplemented in `indicator_platform.c` on
`esp_timer_get_time()`.

## RP2040 & out-of-scope

**RP2040: permanently out of scope** (user decision). Its environmental sensors
(SCD41/ATH20/SGP40) are unrelated to a Claude-usage pet; it keeps running stock
factory firmware, unqueried, and `indicator_platform` does **not** reserve the
UART link. Its beep/power command surface is not integrated.

Explicitly deferred, not silently dropped: `ble_bridge.*`/`-DBUDDY_BLE`
(WiFi-only migration), GIF character packs (`character.*`, `xfer.h`, `b64.h`,
`-DBUDDY_GIF`), and `-DMOCK_DATA` emulator support for this target.

## Risks & de-risking spikes

Ordered by how early they gate the plan; each is a standalone flashable probe in
the spirit of the repo's existing `wifi_probe`/`ble_probe`:

1. **`indicator_bringup_probe`** — build and flash the stock
   `SenseCAP_Indicator_ESP32` display+touch example unmodified. Confirms the
   vendored BSP works and pins the exact ESP-IDF/LVGL versions.
2. **`indicator_heap_probe`** — `bsp_board` init + canvas alloc + WiFi join +
   esp-mqtt connect, logging heap/PSRAM stats. Confirms display+WiFi+MQTT fit
   concurrently.
3. **`indicator_mqtt_probe`** — subscribe to one real SenseCraft measurement
   topic on the NEW device EUI, log raw esp-mqtt event boundaries through the
   reassembly buffer, confirm the portable parser sees whole JSON. Doubles as
   the sensecraft-cli device-creation dry run.
4. **`indicator_font_probe`** — render one species' idle frame at 2–3 candidate
   cell sizes on hardware; eyeball arm's-length legibility before locking the
   canvas grid.
5. **`indicator_jsonlib_probe`** — compile `data.h` unmodified against the shim
   headers (`Arduino.h` mini-shim, forwarding `wio_platform.h`, stub `xfer.h`)
   plus vendored ArduinoJson under ESP-IDF, confirming it needs no source changes.
6. **`indicator_nvs_timing_probe`** — measure a single NVS commit's blocking
   duration on this flash part; confirm dispatch off the LVGL task is sufficient.

## Milestones

- **M0 — Bring-up (2–3 days)**: spikes 1–2 pass; ESP-IDF/LVGL versions locked;
  CI compiles the stock BSP example. *Testable: touch registers; heap numbers in hand.*
- **M1 — Static UI shell (3–4 days)**: `lv_tabview` with 4 empty screens +
  status bar; `buddy_core`/diff-guard wired in. *Testable: navigable tab UI.*
- **M2 — Buddy engine port (4–5 days)**: one species idling on the 8-bit
  `lv_canvas`, driven by `data.h` demo mode (no network); touch-pet mapping
  (tap = heart, scrub = dizzy) finalized against the Wio handler table.
  *Testable: pet animates and reacts to touch on-device.*
- **M3 — Transport port (3–4 days)**: `net_bridge_core` + esp-mqtt shell +
  reassembly buffer, NVS-image creds, new sensecraft-cli device, real
  heartbeat/usage from a live channel into unchanged `data.h`, verified against
  an unmodified `buddy_sensecraft_bridge.py`. *Testable: real usage data on the pet.*
- **M4 — Provisioning UI (5–6 days)**: scan/keyboard/NVS/boot-FSM/
  Disconnect-vs-Forget, replacing the flashed WiFi creds. *Testable: full
  boot-to-connected flow with no USB intervention.*
- **M5 — Panes + polish (3–4 days)**: Sessions/Pet-Stats/Usage with real data,
  approve/deny buttons (hidden path), rescan guard, diff-guard covering all
  shared files in CI.

## Resolved decisions (2026-07-09)

1. Accelerometer gestures → **touch-the-pet** (tap = affection/heart,
   scrub = dizzy, nap = idle timer only).
2. RP2040 and its sensors → **permanently out of scope**; UART link not reserved.
3. Remembered-network cap → **5 confirmed**.
4. SenseCraft identity → **new device, provisioned via sensecraft-cli**; a
   single host bridge process uplinks to **both devices at once** (multi-device
   `devices` list in config, backwards-compatible) — no separate host version.
