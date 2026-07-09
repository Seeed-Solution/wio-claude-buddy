// Phase-1 probe: prove the Wio's shared headers run VERBATIM under ESP-IDF.
// (spike 5 of the design doc, plus the software half of spike 3)
//
// Feeds canned SenseCraft OpenStream publishes through net_bridge_core —
// exactly what the esp-mqtt event handler will do in Phase 2 — waits out the
// debounce, and lets data.h's untouched parser consume the synthesized JSON
// lines off the tr* ring. Success = the parsed TamaState/UsageState match
// the fed measurements on the serial log.
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "net_bridge_core.h"   // shared wire-format core (diff-guarded copy)
#include "stats.h"             // shared verbatim (declares statsOnBridgeTokens)
#include "data.h"              // shared VERBATIM — the point of this probe

static const char* TAG = "probe";

static void feedMeas(int id, double v, uint64_t tsMs) {
  char topic[96], body[96];
  snprintf(topic, sizeof(topic),
           "/device_sensor_data/999/2CF7F1C0TEST00AA/1/vs/%d", id);
  snprintf(body, sizeof(body), "{\"value\":%.0f,\"timestamp\":\"%llu\"}",
           v, (unsigned long long)tsMs);
  nbcOnPayload(topic, (const uint8_t*)body, strlen(body), millis());
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "claude_buddy_indicator phase-1 probe (data.h verbatim)");
  statsLoad();

  const uint64_t TS = 1751970000000ULL;
  // heartbeat group (4097-4101) + usage group (4102-4106) + biased tz (4108)
  feedMeas(4097, 3, TS); feedMeas(4098, 2, TS); feedMeas(4099, 1, TS);
  feedMeas(4100, 12345, TS); feedMeas(4101, 678, TS);
  feedMeas(4102, 42, TS); feedMeas(4103, 3600, TS);
  feedMeas(4104, 17, TS); feedMeas(4105, 90000, TS); feedMeas(4106, 55555, TS);
  feedMeas(4108, 115200, TS);   // tz_offset 28800 (UTC+8) + 86400 bias

  vTaskDelay(pdMS_TO_TICKS(600));   // let the 500 ms debounce expire
  nbcTick(millis());                // synthesize JSON into the tr* ring

  TamaState st = {};
  dataPoll(&st);                    // data.h drains the ring, unmodified

  RTC_TimeTypeDef t; rtcGetTime(&t);
  const UsageState& u = dataUsage();
  ESP_LOGI(TAG, "heartbeat: total=%u running=%u waiting=%u msg='%s' tokToday=%" PRIu32,
           st.sessionsTotal, st.sessionsRunning, st.sessionsWaiting,
           st.msg, st.tokensToday);
  ESP_LOGI(TAG, "usage: have=%d session=%d%%/%lds week=%d%%/%lds today=%ld",
           u.have, u.sessPct, u.sessResetS, u.weekPct, u.weekResetS, u.todayTokens);
  ESP_LOGI(TAG, "rtc: %02u:%02u:%02u valid=%d (expect 08:20:00 local, UTC+8)",
           t.Hours, t.Minutes, t.Seconds, dataRtcValid());

  bool ok = st.sessionsTotal == 3 && st.sessionsRunning == 2 &&
            st.sessionsWaiting == 1 && st.tokensToday == 678 &&
            u.have && u.sessPct == 42 && u.weekPct == 17 &&
            u.todayTokens == 55555 && dataRtcValid();
  ESP_LOGI(TAG, "%s", ok ? "PROBE OK — wire format + data.h verified on ESP-IDF"
                         : "PROBE FAILED — see fields above");

  // keep-alive so the log stays visible on a monitor
  while (true) vTaskDelay(pdMS_TO_TICKS(10000));
}
