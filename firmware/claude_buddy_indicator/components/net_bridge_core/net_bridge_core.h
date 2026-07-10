#pragma once
// Platform-free core of the SenseCraft OpenStream transport: measurement
// accumulation, debounce, JSON-line synthesis, and the RX ring buffer.
//
// This file is the SHARED WIRE-FORMAT CONTRACT between hardware targets.
// It is #included by exactly one translation unit per program:
//   - Wio:       net_bridge.cpp                     (rpcWiFi + PubSubClient)
//   - Indicator: net_bridge_shell.c                 (esp-mqtt)
//   - native:    test/test_net_bridge_core.cpp      (unit tests, no board)
// The Indicator build keeps a byte-identical copy under
// firmware/claude_buddy_indicator/components/net_bridge_core/ enforced by
// scripts/check_shared_files.sh — edit BOTH copies or CI fails.
//
// Deliberately pure C89-style C over <stdint.h>/<string.h>/<stdio.h>/
// <stdlib.h>: no Arduino, no ESP-IDF, no millis() — callers pass nowMs
// (any monotonic ms clock). No ArduinoJson either: the downlink payload is
// a tiny fixed shape ({"value":N,"timestamp":<ms>}), parsed with
// strstr/strtod, keeping heap churn and header risk out of radio TUs.
//
// Measurement map (channel "1"; must match buddy_sensecraft_bridge.py — the
// platform silently drops custom/unknown measurement IDs, so standard IDs
// are reused):
//   4097 total          4102 session.pct      4106 today.tokens
//   4098 running        4103 session.reset_s  4108 tz_offset_sec + 86400
//   4099 waiting        4104 week.pct              (biased so the wire never
//   4100 tokens         4105 week.reset_s           carries a negative)
//   4101 tokens_today
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── RX ring buffer (same pattern as ble_bridge.cpp; drained by dataPoll) ──
#define NBC_RX_CAP 2048
static uint8_t nbcRxBuf[NBC_RX_CAP];
static volatile size_t nbcRxHead = 0, nbcRxTail = 0;

static void nbcRxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t nx = (nbcRxHead + 1) % NBC_RX_CAP;
    if (nx == nbcRxTail) return;   // full — drop (reader should keep up)
    nbcRxBuf[nbcRxHead] = p[i];
    nbcRxHead = nx;
  }
}

static void nbcRxPushLine(const char* s) {
  nbcRxPush((const uint8_t*)s, strlen(s));
  nbcRxPush((const uint8_t*)"\n", 1);
}

static size_t nbcAvailable(void) {
  return (nbcRxHead + NBC_RX_CAP - nbcRxTail) % NBC_RX_CAP;
}

static int nbcRead(void) {
  if (nbcRxHead == nbcRxTail) return -1;
  int b = nbcRxBuf[nbcRxTail];
  nbcRxTail = (nbcRxTail + 1) % NBC_RX_CAP;
  return b;
}

// ── measurement accumulator ──
// Index = measurement ID - 4097 (4108 folds to index 10; 4107 unused).
#define NBC_N_MEAS 11
static const uint32_t NBC_TZ_BIAS = 86400;
static double   nbcMeas[NBC_N_MEAS] = {0};
static int      nbcSeen[NBC_N_MEAS] = {0};
static uint64_t nbcLastTsMs = 0;      // host wall-clock ms from any payload

// Two flush groups: heartbeat (idx 0-4) and usage (idx 5-9). tz (idx 10)
// rides along with the time-sync line.
typedef struct { uint32_t lastRxMs; uint32_t dirtySinceMs; int dirty; } NbcGroup;
static NbcGroup nbcGHb = {0, 0, 0}, nbcGUs = {0, 0, 0};
#define NBC_FLUSH_QUIET_MS 500u       /* flush after the burst settles   */
#define NBC_FLUSH_MAX_MS   2500u      /* ...but never later than this    */
static uint32_t nbcLastTimeSyncMs = 0;  // re-sync soft RTC every 10 min

static int nbcMeasIndex(long id) {
  if (id >= 4097 && id <= 4106) return (int)(id - 4097);
  if (id == 4108) return 10;
  return -1;
}

// Feed one OpenStream publish. measurement ID = last topic segment; the
// channel (3rd segment) is ignored — the host uplinks everything on
// channel 1. nowMs is the caller's monotonic clock.
static void nbcOnPayload(const char* topic, const uint8_t* payload,
                         size_t length, uint32_t nowMs) {
  const char* seg = strrchr(topic, '/');
  if (!seg) return;
  long id = strtol(seg + 1, NULL, 10);
  int idx = nbcMeasIndex(id);
  if (idx < 0) return;

  char body[96];
  if (length >= sizeof(body)) return;      // not ours — payloads are tiny
  memcpy(body, payload, length);
  body[length] = 0;

  const char* v = strstr(body, "\"value\":");
  if (!v) return;
  v += 8;
  if (*v == '"') v++;                      // tolerate quoted numbers
  nbcMeas[idx] = strtod(v, NULL);
  nbcSeen[idx] = 1;
  const char* t = strstr(body, "\"timestamp\":");
  if (t) {
    t += 12;
    if (*t == '"') t++;                    // broker sends it as a quoted string
    uint64_t ts = strtoull(t, NULL, 10);
    if (ts > 1000000000000ULL) nbcLastTsMs = ts;
  }

  NbcGroup* g = (idx <= 4) ? &nbcGHb : (idx <= 9) ? &nbcGUs : NULL;
  if (g) {
    g->lastRxMs = nowMs;
    if (!g->dirty) { g->dirty = 1; g->dirtySinceMs = nowMs; }
  }
}

// ── JSON synthesis (the same lines the BLE bridge used to send) ──
static void nbcFlushTimeSync(uint32_t nowMs) {
  if (!nbcSeen[10] || nbcLastTsMs == 0) return;
  if (nbcLastTimeSyncMs != 0 && nowMs - nbcLastTimeSyncMs < 600000UL) return;
  long off = (long)nbcMeas[10] - (long)NBC_TZ_BIAS;
  char line[64];
  snprintf(line, sizeof(line), "{\"time\":[%lu,%ld]}",
           (unsigned long)(nbcLastTsMs / 1000ULL), off);
  nbcRxPushLine(line);
  nbcLastTimeSyncMs = nowMs;
}

static void nbcFlushHeartbeat(void) {
  long total   = (long)nbcMeas[0], running = (long)nbcMeas[1];
  long waiting = (long)nbcMeas[2];
  long tokens  = (long)nbcMeas[3], tokToday = (long)nbcMeas[4];
  char msg[24];
  if (running > 0) snprintf(msg, sizeof(msg), "%ld active", running);
  else             snprintf(msg, sizeof(msg), "idle");
  char line[160];
  snprintf(line, sizeof(line),
           "{\"total\":%ld,\"running\":%ld,\"waiting\":%ld,\"msg\":\"%s\","
           "\"tokens\":%ld,\"tokens_today\":%ld}",
           total, running, waiting, msg, tokens, tokToday);
  nbcRxPushLine(line);
}

static void nbcFlushUsage(void) {
  char line[160];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"session\":{\"pct\":%ld,\"reset_s\":%ld},"
           "\"week\":{\"pct\":%ld,\"reset_s\":%ld},\"today\":{\"tokens\":%ld}}",
           (long)nbcMeas[5], (long)nbcMeas[6], (long)nbcMeas[7],
           (long)nbcMeas[8], (long)nbcMeas[9]);
  nbcRxPushLine(line);
}

static int nbcGroupDue(const NbcGroup* g, uint32_t nowMs) {
  return g->dirty && (nowMs - g->lastRxMs >= NBC_FLUSH_QUIET_MS ||
                      nowMs - g->dirtySinceMs >= NBC_FLUSH_MAX_MS);
}

// Debounce check + flush; call every loop iteration while connected.
static void nbcTick(uint32_t nowMs) {
  if (nbcGroupDue(&nbcGHb, nowMs)) {
    nbcGHb.dirty = 0;
    nbcFlushHeartbeat();
    nbcFlushTimeSync(nowMs);
  }
  if (nbcGroupDue(&nbcGUs, nowMs)) {
    nbcGUs.dirty = 0;
    nbcFlushUsage();
  }
}
