// SenseCAP Indicator Claude Buddy — Phase 2.
//
// Boot: self-test (canned wire data through the shared core + verbatim
// data.h), then display (vendor BSP + LVGL), then WiFi + esp-mqtt.
// UI: lv_tabview — Pet (shared ASCII buddy engine on a canvas, touch to
// interact: tap = heart, scrub = dizzy), Usage (session/week bars),
// Sessions (counts). The pet + wire parsing are byte-identical to the Wio
// firmware (diff-guarded); only this file and the shims are Indicator-own.
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp_board.h"
#include "lv_port.h"
#include "lvgl.h"

#include "net_bridge_shell.h"  // owns net_bridge_core (single-TU rule)
#include "wifi_provision.h"    // WiFi lifecycle + scan/keyboard/NVS tab
#include "stats.h"             // shared verbatim (declares statsOnBridgeTokens)
#include "data.h"              // shared VERBATIM
#include "buddy.h"             // shared VERBATIM — the ASCII pet engine
#include "buddy_canvas.h"      // its LVGL render target

static const char* TAG = "buddy";

// Mirrors the Wio .ino's PersonaState (buddy.h: states[7] in this order).
enum PersonaState : uint8_t {
  P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART
};

// ── persona derivation (same rules as the Wio's derive()) + touch one-shots ──
static volatile uint32_t sTapCount = 0;       // bumped in the LVGL task
static uint32_t sOneShotUntil = 0;
static PersonaState sOneShot = P_IDLE;

static PersonaState derive(const TamaState& s) {
  if (!s.connected)           return P_IDLE;
  if (s.sessionsWaiting > 0)  return P_ATTENTION;
  if (s.recentlyCompleted)    return P_CELEBRATE;
  if (s.sessionsRunning >= 1) return P_BUSY;
  return P_IDLE;
}

static void onPetTouched(lv_event_t*) { sTapCount++; }

// tap = affection (heart); ≥4 taps in a 2 s window = scrub (dizzy)
static void pollTouch(uint32_t now) {
  static uint32_t seen = 0, windowStart = 0, windowTaps = 0;
  uint32_t taps = sTapCount;
  if (taps == seen) return;
  uint32_t newTaps = taps - seen;
  seen = taps;
  if (now - windowStart > 2000) { windowStart = now; windowTaps = 0; }
  windowTaps += newTaps;
  if (windowTaps >= 4) {
    sOneShot = P_DIZZY; sOneShotUntil = now + 4000;
    ESP_LOGI(TAG, "touch: scrub -> dizzy");
  } else if (sOneShotUntil <= now || sOneShot != P_DIZZY) {
    sOneShot = P_HEART; sOneShotUntil = now + 3000;
    ESP_LOGI(TAG, "touch: pet -> heart");
  }
}

// ── UI ──
static lv_obj_t *sStatus, *sMsg, *sSessBar, *sSessLbl, *sWeekBar, *sWeekLbl,
                *sCounts, *sSpecies;

static lv_obj_t* makeBar(lv_obj_t* parent, const char* name, lv_color_t color,
                         int y, lv_obj_t** valueLbl) {
  lv_obj_t* cap = lv_label_create(parent);
  lv_label_set_text(cap, name);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(cap, lv_color_hex(0x9aa0a6), 0);
  lv_obj_set_pos(cap, 20, y);

  lv_obj_t* bar = lv_bar_create(parent);
  lv_obj_set_size(bar, 400, 26);
  lv_obj_set_pos(bar, 20, y + 34);
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

static void styleTab(lv_obj_t* tab) {
  lv_obj_set_style_bg_color(tab, lv_color_hex(0x121417), 0);
  lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* sTabview;

static void uiInit(void) {
  lv_obj_t* tv = sTabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 52);
  lv_obj_set_style_bg_color(tv, lv_color_hex(0x121417), 0);
  lv_obj_t* petTab  = lv_tabview_add_tab(tv, "Pet");
  lv_obj_t* useTab  = lv_tabview_add_tab(tv, "Usage");
  lv_obj_t* sessTab = lv_tabview_add_tab(tv, "Sessions");
  lv_obj_t* wifiTab = lv_tabview_add_tab(tv, "WiFi");
  styleTab(petTab); styleTab(useTab); styleTab(sessTab); styleTab(wifiTab);
  wpCreateTab(wifiTab);

  // ── Pet tab: the buddy canvas (2x engine scale, 2x zoom -> 300x380) ──
  lv_obj_t* canvas = buddyCanvasCreate(petTab);
  lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, -60);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, onPetTouched, LV_EVENT_PRESSED, NULL);

  sMsg = lv_label_create(petTab);
  lv_label_set_text(sMsg, "");
  lv_obj_set_style_text_font(sMsg, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sMsg, lv_color_white(), 0);
  lv_obj_align(sMsg, LV_ALIGN_BOTTOM_MID, 0, -66);

  sStatus = lv_label_create(petTab);
  lv_label_set_text(sStatus, "starting...");
  lv_obj_set_style_text_font(sStatus, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sStatus, lv_color_hex(0x9aa0a6), 0);
  lv_obj_align(sStatus, LV_ALIGN_BOTTOM_MID, 0, -30);

  sSpecies = lv_label_create(petTab);
  lv_label_set_text(sSpecies, "");
  lv_obj_set_style_text_font(sSpecies, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sSpecies, lv_color_hex(0x5f6368), 0);
  lv_obj_align(sSpecies, LV_ALIGN_TOP_RIGHT, -12, 6);

  // ── Usage tab ──
  lv_obj_t* title = lv_label_create(useTab);
  lv_label_set_text(title, "Claude Buddy");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xD97757), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  sSessBar = makeBar(useTab, "SESSION", lv_color_hex(0x34a853), 110, &sSessLbl);
  sWeekBar = makeBar(useTab, "WEEK",    lv_color_hex(0x4285f4), 210, &sWeekLbl);

  // ── Sessions tab ──
  sCounts = lv_label_create(sessTab);
  lv_label_set_text(sCounts, "");
  lv_obj_set_style_text_font(sCounts, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(sCounts, lv_color_white(), 0);
  lv_obj_align(sCounts, LV_ALIGN_TOP_LEFT, 20, 20);
}

static void uiUpdate(const TamaState& st) {
  const UsageState& u = dataUsage();
  lv_label_set_text_fmt(sStatus, "wifi+mqtt %s  |  %s",
                        trConnected() ? "connected" : "connecting...",
                        st.connected ? "Claude live" : "no data");
  lv_label_set_text_fmt(sMsg, "%s", st.msg);
  lv_label_set_text_fmt(sSpecies, "%s", buddySpeciesName());
  lv_bar_set_value(sSessBar, u.have ? u.sessPct : 0, LV_ANIM_ON);
  lv_label_set_text_fmt(sSessLbl, "%d%%", u.have ? u.sessPct : 0);
  lv_bar_set_value(sWeekBar, u.have ? u.weekPct : 0, LV_ANIM_ON);
  lv_label_set_text_fmt(sWeekLbl, "%d%%", u.have ? u.weekPct : 0);
  lv_label_set_text_fmt(sCounts,
      "sessions\n\n%u total\n%u running\n%u waiting\n\ntokens today: %" PRIu32,
      st.sessionsTotal, st.sessionsRunning, st.sessionsWaiting, st.tokensToday);
}

// ── boot self-test (spike 5, kept as a permanent regression check) ──
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
  injectMeas(4097, 3, TS); injectMeas(4098, 2, TS); injectMeas(4099, 1, TS);
  injectMeas(4100, 12345, TS); injectMeas(4101, 678, TS);
  injectMeas(4102, 42, TS); injectMeas(4103, 3600, TS);
  injectMeas(4104, 17, TS); injectMeas(4105, 90000, TS);
  injectMeas(4106, 55555, TS);
  injectMeas(4108, 115200, TS);
  vTaskDelay(pdMS_TO_TICKS(600));
  trLoop();
  dataPoll(st);
  const UsageState& u = dataUsage();
  return st->sessionsTotal == 3 && st->sessionsRunning == 2 &&
         st->sessionsWaiting == 1 && st->tokensToday == 678 &&
         u.have && u.sessPct == 42 && u.weekPct == 17 &&
         u.todayTokens == 55555 && dataRtcValid();
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "claude_buddy_indicator phase-2 (pet + tabs + touch)");
  statsLoad();

  TamaState st = {};
  ESP_LOGI(TAG, "%s", selfTest(&st) ? "SELF-TEST OK" : "SELF-TEST FAILED");

  ESP_ERROR_CHECK(bsp_board_init());
  lv_port_init();
  buddyInit();
  buddySetPeek(false);              // 2x engine scale (Wio home-screen look)

  wpStart();                        // WiFi: NVS profiles + boot FSM
  trInit("Claude Indicator");       // MQTT (starts when WiFi gets an IP)

  lv_port_sem_take();
  uiInit();
  if (!wpHasProfiles())             // unprovisioned: open the WiFi tab
    lv_tabview_set_act(sTabview, 3, LV_ANIM_OFF);
  lv_port_sem_give();
  ESP_LOGI(TAG, "display up — species '%s', %s", buddySpeciesName(),
           wpHasProfiles() ? "known network(s) in NVS" : "unprovisioned");

  uint32_t lastReport = 0, lastUi = 0;
  while (true) {
    trLoop();
    dataPoll(&st);
    uint32_t now = millis();
    pollTouch(now);

    PersonaState p = derive(st);
    if (now < sOneShotUntil) p = sOneShot;

    lv_port_sem_take();
    buddyTick((uint8_t)p);          // engine draws into the canvas buffer
    buddyCanvasFlush();
    wpLoop(now);                    // WiFi FSM + WiFi-tab refresh
    if (now - lastUi >= 500) { lastUi = now; uiUpdate(st); }
    lv_port_sem_give();

    if (now - lastReport >= 30000) {
      lastReport = now;
      const UsageState& u = dataUsage();
      ESP_LOGI(TAG, "%s mqtt=%d total=%u running=%u '%s' session=%d%% week=%d%% persona=%d",
               st.connected ? "live" : "asleep", trConnected(),
               st.sessionsTotal, st.sessionsRunning, st.msg,
               u.sessPct, u.weekPct, (int)p);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
