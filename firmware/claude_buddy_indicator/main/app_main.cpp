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

#include "bsp_board.h"
#include "lv_port.h"
#include "lvgl.h"

#include "net_bridge_shell.h"  // owns net_bridge_core (single-TU rule)
#include "stats.h"             // shared verbatim (declares statsOnBridgeTokens)
#include "data.h"              // shared VERBATIM — the point of this port

static const char* TAG = "buddy";

// ── Phase-2 preview status screen ──
// A plain LVGL page (title, connection line, session/week bars, session
// counts) fed by the same TamaState/UsageState the Wio panes read. The pet
// canvas and tabview replace this in milestones M1/M2.
static lv_obj_t *sTitle, *sStatus, *sMsg, *sSessBar, *sSessLbl, *sWeekBar,
                *sWeekLbl, *sCounts;

static lv_obj_t* makeBar(lv_obj_t* parent, const char* name, lv_color_t color,
                         int y, lv_obj_t** valueLbl) {
  lv_obj_t* cap = lv_label_create(parent);
  lv_label_set_text(cap, name);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(cap, lv_color_hex(0x9aa0a6), 0);
  lv_obj_set_pos(cap, 40, y);

  lv_obj_t* bar = lv_bar_create(parent);
  lv_obj_set_size(bar, 400, 26);
  lv_obj_set_pos(bar, 40, y + 30);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x2a2d31), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);

  *valueLbl = lv_label_create(parent);
  lv_label_set_text(*valueLbl, "--%");
  lv_obj_set_style_text_font(*valueLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(*valueLbl, lv_color_white(), 0);
  lv_obj_set_pos(*valueLbl, 350, y);
  return bar;
}

static void uiInit(void) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x121417), 0);

  sTitle = lv_label_create(scr);
  lv_label_set_text(sTitle, "Claude Buddy");
  lv_obj_set_style_text_font(sTitle, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(sTitle, lv_color_hex(0xD97757), 0);
  lv_obj_align(sTitle, LV_ALIGN_TOP_MID, 0, 28);

  sStatus = lv_label_create(scr);
  lv_label_set_text(sStatus, "starting...");
  lv_obj_set_style_text_font(sStatus, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(sStatus, lv_color_hex(0x9aa0a6), 0);
  lv_obj_align(sStatus, LV_ALIGN_TOP_MID, 0, 84);

  sMsg = lv_label_create(scr);
  lv_label_set_text(sMsg, "");
  lv_obj_set_style_text_font(sMsg, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sMsg, lv_color_white(), 0);
  lv_obj_align(sMsg, LV_ALIGN_TOP_MID, 0, 130);

  sSessBar = makeBar(scr, "SESSION", lv_color_hex(0x34a853), 200, &sSessLbl);
  sWeekBar = makeBar(scr, "WEEK",    lv_color_hex(0x4285f4), 290, &sWeekLbl);

  sCounts = lv_label_create(scr);
  lv_label_set_text(sCounts, "");
  lv_obj_set_style_text_font(sCounts, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(sCounts, lv_color_hex(0x9aa0a6), 0);
  lv_obj_align(sCounts, LV_ALIGN_BOTTOM_MID, 0, -60);

  lv_obj_t* foot = lv_label_create(scr);
  lv_label_set_text(foot, "phase 2 preview - pet UI coming");
  lv_obj_set_style_text_font(foot, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x5f6368), 0);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void uiUpdate(const TamaState& st) {
  const UsageState& u = dataUsage();
  lv_label_set_text_fmt(sStatus, "wifi+mqtt %s   |   %s",
                        trConnected() ? "connected" : "connecting...",
                        st.connected ? "Claude live" : "no data");
  lv_label_set_text_fmt(sMsg, "%s", st.msg);
  lv_bar_set_value(sSessBar, u.have ? u.sessPct : 0, LV_ANIM_ON);
  lv_label_set_text_fmt(sSessLbl, "%d%%", u.have ? u.sessPct : 0);
  lv_bar_set_value(sWeekBar, u.have ? u.weekPct : 0, LV_ANIM_ON);
  lv_label_set_text_fmt(sWeekLbl, "%d%%", u.have ? u.weekPct : 0);
  lv_label_set_text_fmt(sCounts, "sessions: %u total   %u running   %u waiting",
                        st.sessionsTotal, st.sessionsRunning, st.sessionsWaiting);
}

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

  ESP_ERROR_CHECK(bsp_board_init());   // display + touch (vendor BSP)
  lv_port_init();
  lv_port_sem_take();
  uiInit();
  lv_port_sem_give();
  ESP_LOGI(TAG, "display up (480x480, LVGL)");

  trInit("Claude Indicator");       // WiFi + MQTT (indicator_secrets.h)

  uint32_t lastReport = 0, lastUi = 0;
  uint8_t lastTotal = 255, lastRunning = 255;
  while (true) {
    trLoop();
    dataPoll(&st);
    uint32_t now = millis();
    if (now - lastUi >= 500) {
      lastUi = now;
      lv_port_sem_take();
      uiUpdate(st);
      lv_port_sem_give();
    }
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
