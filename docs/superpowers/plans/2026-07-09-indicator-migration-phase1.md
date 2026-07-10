# SenseCAP Indicator Migration — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multi-device host uplink, extraction of the platform-free
`net_bridge_core` with native tests, CI diff-guard, and a compile-verified
ESP-IDF scaffold for the Indicator — everything testable without hardware.

**Architecture:** Follows `docs/superpowers/specs/2026-07-09-sensecap-indicator-migration-design.md`.
The Wio build stays byte-compatible; the portable core is a single-include
`net_bridge_core.h` used by exactly one TU per program (Wio `net_bridge.cpp`,
Indicator `net_bridge_shell.c`, native test). LVGL UI / provisioning are Phase 2,
gated on the hardware spike results by design.

**Tech Stack:** Python 3.9 stdlib (host), Arduino C++ (Wio, Seeeduino:samd),
ESP-IDF pinned to Seeed's `SenseCAP_Indicator_ESP32` BSP version, g++ native tests.

**Out of scope (Phase 2):** LVGL tabview UI, buddy canvas rendering, WiFi
provisioning screens, NVS profiles — these depend on spike results from real
hardware (M0) and get their own plan.

---

### Task 1: Host — `normalize_config` with multi-device support (TDD)

**Files:**
- Modify: `host/buddy_sensecraft_bridge.py` (config section, ~lines 53–86)
- Create: `host/test_sensecraft_config.py`
- Modify: `host/sensecraft_config.example.json`

- [ ] **Step 1: Write the failing test** (`host/test_sensecraft_config.py`):

```python
from __future__ import annotations
import unittest
from buddy_sensecraft_bridge import normalize_config, DEFAULT_API_BASE

class TestNormalizeConfig(unittest.TestCase):
    def test_env_overrides_everything(self):
        cfg = normalize_config({"devices": [{"eui": "AAA", "key": "k"}]},
                               {"SENSECRAFT_DEVICE_EUI": "EEE",
                                "SENSECRAFT_DEVICE_KEY": "KKK"})
        self.assertEqual(cfg["devices"], [{"eui": "EEE", "key": "KKK"}])

    def test_devices_list(self):
        cfg = normalize_config(
            {"devices": [{"eui": "A", "key": "ka"}, {"eui": "B", "key": "kb"}]}, {})
        self.assertEqual([d["eui"] for d in cfg["devices"]], ["A", "B"])

    def test_legacy_single_device(self):
        cfg = normalize_config({"eui": "A", "key": "ka"}, {})
        self.assertEqual(cfg["devices"], [{"eui": "A", "key": "ka"}])

    def test_defaults(self):
        cfg = normalize_config({"eui": "A", "key": "k"}, {})
        self.assertEqual(cfg["api_base"], DEFAULT_API_BASE)
        self.assertEqual(cfg["interval_s"], 10)

    def test_missing_creds_raises(self):
        with self.assertRaises(SystemExit):
            normalize_config({}, {})

    def test_incomplete_device_raises(self):
        with self.assertRaises(SystemExit):
            normalize_config({"devices": [{"eui": "A"}]}, {})

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2:** `cd host && python3 -m unittest test_sensecraft_config -v` → FAIL (no `normalize_config`)
- [ ] **Step 3:** Implement `normalize_config(raw, env)` in the bridge; `load_config()` becomes `normalize_config(json-from-file, os.environ)`. Precedence: env vars → `devices` list → legacy `eui`/`key`; `SystemExit` with the provisioning hint when empty/incomplete.
- [ ] **Step 4:** Re-run unittest → 6 PASS. Also `python3 -m py_compile buddy_sensecraft_bridge.py test_sensecraft_config.py`.
- [ ] **Step 5:** Update `sensecraft_config.example.json` to show `devices` (comment the legacy form in the README of the key). Commit: `feat(host): multi-device config normalization`.

### Task 2: Host — uplink loop posts to every device

**Files:** Modify `host/buddy_sensecraft_bridge.py` (`uplink_body`, `post_uplink`, `run`), docstring.

- [ ] **Step 1:** `uplink_body(dev, meas, declare_channel)` / `post_uplink(api_base, dev, body)` take a device dict. Per-device runtime state lives on the dict: `declared`, `backoff`, `next_try` (init 0). In `run()`, each cycle iterates `cfg["devices"]`; a device is skipped while `now < next_try`; success → `declared=True, backoff=0`; failure → `backoff = min(60, backoff*2) or 5`, `next_try = now + backoff`, log per-EUI. `time.sleep(interval_s)` always (per-device backoff replaces the global one). Dry-run prints one body per device.
- [ ] **Step 2:** Verify: `SENSECRAFT_DEVICE_EUI=TESTEUI SENSECRAFT_DEVICE_KEY=k python3 buddy_sensecraft_bridge.py --dry-run` prints a valid body with `"deviceEui": "TESTEUI"`; then a two-device `devices` config via a temp `SENSECRAFT_CONFIG` (add env override for the config path if not present) prints two bodies.
- [ ] **Step 3:** `python3 -m py_compile` all host files (CI lint parity). Commit: `feat(host): uplink to multiple SenseCraft devices from one process`.

### Task 3: Firmware — extract `net_bridge_core.h`

**Files:**
- Create: `firmware/claude_buddy/net_bridge_core.h`
- Modify: `firmware/claude_buddy/net_bridge.cpp`

- [ ] **Step 1:** Move verbatim from `net_bridge.cpp` into `net_bridge_core.h`: the RX ring buffer (lines 21–38), measurement accumulator + `measIndex` (40–60), `Group`/`groupDue` (50–53, 145–148), payload parsing from `onMessage` body (83–107, minus the MQTT types), and the three `flush*` functions (110–143). Public (static) API, all pure C over `<stdint.h> <stddef.h> <string.h> <stdio.h> <stdlib.h>`:

```c
static void   nbcOnPayload(const char* topic, const uint8_t* payload,
                           size_t length, uint32_t nowMs);
static void   nbcTick(uint32_t nowMs);       // debounce check + flush*
static size_t nbcAvailable(void);
static int    nbcRead(void);
```

  `Serial`, `millis()`, WiFi/MQTT types must not appear — callers pass `nowMs`.
- [ ] **Step 2:** `net_bridge.cpp` keeps only the rpcWiFi/PubSubClient state machine; `onMessage` → `nbcOnPayload(topic, payload, length, millis())`; `NS_RUN` flush block → `nbcTick(now)`; `netAvailable/netRead` → `nbcAvailable/nbcRead`.
- [ ] **Step 3:** Sanity: `g++ -std=c++17 -fsyntax-only -x c++ firmware/claude_buddy/net_bridge_core.h` compiles clean natively (proves platform-freedom). Commit: `refactor(firmware): extract platform-free net_bridge_core`.

### Task 4: Native unit test for the core

**Files:** Create `firmware/claude_buddy/test/test_net_bridge_core.cpp`.

- [ ] **Step 1:** Test includes the header directly and drives fake time. Cases: (a) heartbeat meas 4097–4101 → after quiet gap, exactly the JSON line `{"total":..,"running":..,...}` with `msg` = `"N active"`/`"idle"`; (b) usage 4102–4106 → `{"v":1,...}` line; (c) continuous updates flush by `FLUSH_MAX_MS`; (d) time sync from biased 4108 + quoted `"timestamp"` → `{"time":[sec,off]}`, suppressed for 10 min after; (e) quoted `"value"` accepted; (f) IDs 4107/9999 ignored; (g) ring drains byte-exact via `nbcRead`. Assert with a `readLine()` helper draining `nbcRead()` to `\n`.
- [ ] **Step 2:** `g++ -std=c++17 firmware/claude_buddy/test/test_net_bridge_core.cpp -o /tmp/test_nbc && /tmp/test_nbc` → all asserts pass, prints `OK`.
- [ ] **Step 3:** Commit: `test(firmware): native tests for net_bridge_core`.

### Task 5: Wio regression build

- [ ] **Step 1:** `secrets.h` is gitignored — copy from the main checkout into the worktree sketch dir (do NOT commit).
- [ ] **Step 2:** `LCD=~/Library/Arduino15/packages/Seeeduino/hardware/samd/1.8.5/libraries/Seeed_Arduino_LCD && arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" firmware/claude_buddy/claude_buddy.ino` → exit 0. Also the base64 test still passes: `g++ -std=c++17 firmware/claude_buddy/test/test_b64.cpp -o /tmp/test_b64 && /tmp/test_b64`.
- [ ] **Step 3:** No commit (build artifact step); fixes fold into Task 3's files if it fails.

### Task 6: BSP clone + pinned ESP-IDF install

- [ ] **Step 1:** `git submodule add https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32 firmware/claude_buddy_indicator/components/sensecap_bsp` (pinned at whatever HEAD resolves; record the SHA).
- [ ] **Step 2:** Discover the required IDF version: `grep -ri "idf" firmware/claude_buddy_indicator/components/sensecap_bsp/README.md .github 2>/dev/null` and inspect `examples/*/CMakeLists.txt` + CI files. Expected: v5.1.x. Record actual → this pins `sdkconfig.defaults` and the CI action tag.
- [ ] **Step 3:** Install matching ESP-IDF (background, long): `git clone -b <ver> --depth 1 --recursive https://github.com/espressif/esp-idf ~/esp-idf && ~/esp-idf/install.sh esp32s3`. Verify: `. ~/esp-idf/export.sh && idf.py --version`.

### Task 7: Indicator project scaffold (compile = spikes 1-build + 5)

**Files (all under `firmware/claude_buddy_indicator/`):**
- Create: `CMakeLists.txt`, `sdkconfig.defaults`, `main/CMakeLists.txt`, `main/app_main.c`
- Create shims: `main/shim/Arduino.h`, `main/shim/wio_platform.h`, `main/shim/xfer.h`, `main/shim/transport.h`, `main/shim/prefs_compat.h`
- Create: `components/buddy_core/` (diff-guarded copies: `data.h`, `stats.h`, `buddy_common.h`, `buddy.h`, `buddy.cpp`, 18 `buddy_sp_*.cpp`), `components/net_bridge_core/net_bridge_core.h` (copy)
- Vendor: `components/arduinojson/` (single-header release + CMakeLists)

- [ ] **Step 1:** Scaffold with `sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32s3"`, OPI PSRAM, 8 MB flash (verify against BSP example's sdkconfig). `app_main.c` v1 = "jsonlib probe": pushes two canned JSON lines (heartbeat + usage) through `data.h`'s `_LineBuf` path via a stubbed transport and logs the parsed fields — proving `data.h` compiles and runs unmodified under IDF against the shims.
- [ ] **Step 2:** Shims (include-path precedence via `main/shim` listed before `buddy_core`): `Arduino.h` provides `millis()` → `esp_timer_get_time()/1000`, `String` absent (verify `data.h` needs none), min/max, `PROGMEM` no-ops; `wio_platform.h` provides the soft-RTC structs/functions backed by a plain counter for the probe; `xfer.h` stub with the two symbols `data.h` references; `transport.h` maps `tr*` to the probe's canned feeder.
- [ ] **Step 3:** `. ~/esp-idf/export.sh && cd firmware/claude_buddy_indicator && idf.py set-target esp32s3 && idf.py build` → exit 0. This is the compile half of spike 1 plus all of spike 5.
- [ ] **Step 4:** Commit: `feat(indicator): ESP-IDF scaffold — data.h compiles unmodified via shim headers`.

### Task 8: Diff-guard + CI

**Files:** Create `scripts/check_shared_files.sh`; modify `.github/workflows/build.yml`.

- [ ] **Step 1:** `check_shared_files.sh`: for each shared file (list in the script: `data.h stats.h buddy_common.h buddy.h buddy.cpp buddy_sp_*.cpp net_bridge_core.h`), `cmp` the `firmware/claude_buddy/` original against the `claude_buddy_indicator` copy; any diff → exit 1 naming the file. Run locally → exit 0.
- [ ] **Step 2:** CI: new job `indicator` using `espressif/esp-idf-ci-action@v1` with `esp_idf_version` = the Task 6 pin, `target: esp32s3`, `path: firmware/claude_buddy_indicator`; plus a `shared-files` step running the guard script; native `test_net_bridge_core` added next to the existing b64 test; host lint now includes `test_sensecraft_config.py` + runs the unittest.
- [ ] **Step 3:** Commit: `ci: indicator IDF build, shared-file diff-guard, host config tests`.

### Task 9: Hardware spike probes (flash-ready deliverables)

- [ ] **Step 1:** Extend `app_main.c` behind a menu of boot stages (mirroring `wifi_probe`'s staged on-screen pattern, serial-only for now): stage A = heap/PSRAM report (spike 2's logging), stage B = WiFi join + esp-mqtt subscribe to the new device's topic with raw event-boundary logging into the reassembly buffer (spike 3), using creds from `nvs` or compile-time test defines.
- [ ] **Step 2:** `idf.py build` → exit 0. Commit: `feat(indicator): heap + mqtt spike stages`.
- [ ] **Step 3:** Document flash/run instructions for the user (they must create the new sensecraft-cli device and plug the Indicator).

### Task 10: Docs + ship

- [ ] **Step 1:** README + CLAUDE.md: Indicator build commands, multi-device host config, diff-guard rule. Self-review plan vs spec.
- [ ] **Step 2:** Push branch, update PR #1 body with implementation status + what needs hardware.

## Self-review notes

- Spec coverage: host multi-device (T1–2), portable core + wire fidelity (T3–4), Wio unchanged (T5), version pinning (T6), shim headers + `data.h` verbatim (T7), diff-guard/CI (T8), spikes 2/3/5 (T7/T9); spikes 4/6 and M1–M5 are hardware-gated → Phase 2 plan.
- Discovery steps (BSP component names, IDF version) carry exact verification commands; scaffold details get locked against the cloned BSP during Task 6/7 — recorded in commits, not left open.
