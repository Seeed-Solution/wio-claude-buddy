// Phase-1 firmware: prove the Wio's shared headers run VERBATIM under
// ESP-IDF, then run the real SenseCraft loop (design doc spikes 2/3/5).
//
// Boot sequence:
//   1. Self-test (spike 5): canned OpenStream publishes through
//      net_bridge_core, debounce waited out, data.h's untouched parser
//      drains the tr* ring — parsed state must match the fed values.
//   2. Network (spikes 2+3): WiFi join + esp-mqtt subscribe with the
//      indicator_secrets.h credentials, heap logged at each stage, then a
//      forever loop feeding live measurements into the same data.h.
// Display, touch and provisioning arrive in Phase 2 on top of exactly this.
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "net_bridge_shell.h"  // owns net_bridge_core (single-TU rule)
#include "stats.h"             // shared verbatim (declares statsOnBridgeTokens)
#include "data.h"              // shared VERBATIM — the point of this port

static const char* TAG = "buddy";

static void injectMeas(int id, double v, uint64_t tsMs) {
  char topic[96], body[96];
  snprintf(topic, sizeof(topic),
           "/device_sensor_data/999/2CF7F1C0TEST00AA/1/vs/%d", id);
  snprintf(body, sizeof(body), "{\"value\":%.0f,\"timestamp\":\"%llu\"}",
           v, (unsigned long long)tsMs);
  nbsInjectPayload(topic, (const uint8_t*)body, strlen(body));
}

static bool selfTest(TamaState* st) {
  const uint64_t TS = 1751970000000ULL;
  // heartbeat group (4097-4101) + usage group (4102-4106) + biased tz (4108)
  injectMeas(4097, 3, TS); injectMeas(4098, 2, TS); injectMeas(4099, 1, TS);
  injectMeas(4100, 12345, TS); injectMeas(4101, 678, TS);
  injectMeas(4102, 42, TS); injectMeas(4103, 3600, TS);
  injectMeas(4104, 17, TS); injectMeas(4105, 90000, TS);
  injectMeas(4106, 55555, TS);
  injectMeas(4108, 115200, TS);   // tz_offset 28800 (UTC+8) + 86400 bias

  vTaskDelay(pdMS_TO_TICKS(600));   // let the 500 ms debounce expire
  trLoop();                         // synthesize JSON into the tr* ring
  dataPoll(st);                     // data.h drains it, unmodified

  const UsageState& u = dataUsage();
  RTC_TimeTypeDef t; rtcGetTime(&t);
  ESP_LOGI(TAG, "heartbeat: total=%u running=%u waiting=%u msg='%s' tokToday=%" PRIu32,
           st->sessionsTotal, st->sessionsRunning, st->sessionsWaiting,
           st->msg, st->tokensToday);
  ESP_LOGI(TAG, "usage: have=%d session=%d%%/%lds week=%d%%/%lds today=%ld",
           u.have, u.sessPct, u.sessResetS, u.weekPct, u.weekResetS, u.todayTokens);
  ESP_LOGI(TAG, "rtc: %02u:%02u:%02u valid=%d (expect 18:20 — 10:20 UTC + 8h tz)",
           t.Hours, t.Minutes, t.Seconds, dataRtcValid());

  return st->sessionsTotal == 3 && st->sessionsRunning == 2 &&
         st->sessionsWaiting == 1 && st->tokensToday == 678 &&
         u.have && u.sessPct == 42 && u.weekPct == 17 &&
         u.todayTokens == 55555 && dataRtcValid();
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "claude_buddy_indicator phase-1 (shared headers verbatim)");
  statsLoad();

  TamaState st = {};
  ESP_LOGI(TAG, "%s", selfTest(&st)
           ? "SELF-TEST OK — wire format + data.h verified on ESP-IDF"
           : "SELF-TEST FAILED — see fields above");

  trInit("Claude Indicator");       // WiFi + MQTT (indicator_secrets.h)

  uint32_t lastReport = 0;
  uint8_t lastTotal = 255, lastRunning = 255;
  while (true) {
    trLoop();
    dataPoll(&st);
    uint32_t now = millis();
    bool changed = st.sessionsTotal != lastTotal || st.sessionsRunning != lastRunning;
    if (changed || now - lastReport >= 30000) {
      lastReport = now;
      lastTotal = st.sessionsTotal; lastRunning = st.sessionsRunning;
      const UsageState& u = dataUsage();
      ESP_LOGI(TAG, "%s mqtt=%d total=%u running=%u '%s' session=%d%% week=%d%%",
               st.connected ? "live" : "asleep", trConnected(),
               st.sessionsTotal, st.sessionsRunning, st.msg,
               u.sessPct, u.weekPct);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
