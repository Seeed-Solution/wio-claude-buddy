/*
 * Claude Desktop Buddy — Wio Terminal port of Links17/claude-desktop-buddy.
 * Drop-in BLE peer to Claude Desktop's Hardware Buddy; landscape split-view.
 *
 * Build (forced Wio LCD lib — see docs/.../wio-firmware-build-gotchas):
 *   LCD=~/Library/Arduino15/packages/Seeeduino/hardware/samd/1.8.5/libraries/Seeed_Arduino_LCD
 *   # live (BLE, real board):
 *   arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" --export-binaries claude_buddy.ino
 *   # mock (canned data, web emulator):
 *   arduino-cli compile --fqbn Seeeduino:samd:seeed_wio_terminal --library "$LCD" \
 *     --build-property "compiler.cpp.extra_flags=-DMOCK_DATA" --export-binaries claude_buddy.ino
 */
#include "wio_platform.h"
#include "transport.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"

// ── persona state machine (mirrors upstream) ──
enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
static const char* STATE_NAMES[] = { "sleep","idle","busy","attention","celebrate","dizzy","heart" };

static TamaState    tama;
static PersonaState baseState = P_SLEEP, activeState = P_SLEEP;
static uint32_t    oneShotUntil = 0;
static char        lastPromptId[40] = "";
static bool        responseSent = false;
static uint32_t    promptArrivedMs = 0;
static uint8_t     msgScroll = 0;
static uint8_t     paneView = 0;        // right pane: 0=sessions/recent 1=pet stats 2=usage
static const uint8_t PANE_COUNT = 3;

// These are referenced by xfer.h via `extern`, so they must be globals
// (external linkage), not file-static.
bool buddyMode = true;
bool gifAvailable = false;

// colors
static uint16_t C_BG, C_PANEL, C_TEXT, C_DIM, C_GREEN, C_AMBER, C_RED;

static PersonaState derive(const TamaState& s) {
  if (!s.connected)           return P_IDLE;
  if (s.sessionsWaiting > 0)  return P_ATTENTION;
  if (s.recentlyCompleted)    return P_CELEBRATE;
  if (s.sessionsRunning >= 1) return P_BUSY;   // any active session → busy
  return P_IDLE;
}
static void triggerOneShot(PersonaState s, uint32_t ms) { activeState = s; oneShotUntil = millis() + ms; }

static void sendCmd(const char* json) {
  Serial.println(json);
  trWrite((const uint8_t*)json, strlen(json));
  trWrite((const uint8_t*)"\n", 1);
}

// ── header ──
static void drawHeader() {
  spr.fillRect(0, 0, SCREEN_W, 26, C_BG);
  spr.setTextSize(1);
  spr.setTextColor(C_TEXT, C_BG);
  const char* nm = ownerName()[0] ? ownerName() : "Claude Buddy";
  spr.drawString(nm, 6, 6);
  char st[24]; snprintf(st, sizeof(st), "[%s]", STATE_NAMES[activeState]);
  spr.setTextColor(C_AMBER, C_BG); spr.drawString(st, 150, 6);
  char tok[32]; snprintf(tok, sizeof(tok), "today %lu", (unsigned long)tama.tokensToday);
  spr.setTextColor(C_DIM, C_BG); spr.drawString(tok, 230, 6);
  spr.fillCircle(SCREEN_W - 8, 8, 4, tama.connected ? C_GREEN : C_RED);
  spr.drawFastHLine(0, 26, SCREEN_W, C_PANEL);
}

// right-pane geometry
static const int PANE_X = 156;
static uint16_t gaugeCol(int pct) { return pct >= 85 ? C_RED : pct >= 60 ? C_AMBER : C_GREEN; }

static void drawBar(int x, int y, int w, int h, int pct, uint16_t col) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  spr.fillRoundRect(x, y, w, h, 2, C_PANEL);
  int fw = (w * pct) / 100;
  if (fw > 0) spr.fillRoundRect(x, y, fw, h, 2, col);
}

static void fmtCountdown(long s, char* out, size_t n) {
  if (s <= 0) { snprintf(out, n, "now"); return; }
  long h = s / 3600, m = (s % 3600) / 60;
  if (h > 0) snprintf(out, n, "%ldh%02ldm", h, m);
  else snprintf(out, n, "%ldm", m ? m : 1);
}

// view 0 — sessions + transcript (from the Hardware Buddy heartbeat)
static void drawSessionsPane() {
  const int X = PANE_X;
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("SESSIONS", X, 32);
  char b[40];
  spr.setTextColor(C_DIM, C_BG);
  snprintf(b, sizeof(b), "total %u", tama.sessionsTotal);    spr.drawString(b, X, 48);
  spr.setTextColor(tama.sessionsRunning ? C_GREEN : C_DIM, C_BG);
  snprintf(b, sizeof(b), "running %u", tama.sessionsRunning); spr.drawString(b, X, 62);
  spr.setTextColor(tama.sessionsWaiting ? C_AMBER : C_DIM, C_BG);
  snprintf(b, sizeof(b), "waiting %u", tama.sessionsWaiting); spr.drawString(b, X, 76);
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("RECENT", X, 98);
  int y = 112;
  if (tama.nLines == 0) {
    spr.setTextColor(C_DIM, C_BG); spr.drawString(tama.msg, X, y);
  } else {
    int shown = 0;
    for (int i = (int)tama.nLines - 1 - msgScroll; i >= 0 && shown < 6; i--, shown++) {
      spr.setTextColor(shown == 0 ? C_TEXT : C_DIM, C_BG);
      char line[24]; snprintf(line, sizeof(line), "%.22s", tama.lines[i]);
      spr.drawString(line, X, y); y += 12;
    }
  }
}

// view 1 — pet stats: mood / fed / energy / level (from stats.h)
static void drawPetStatsPane() {
  const int X = PANE_X;
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("PET STATS", X, 32);
  // mood — up to 4 hearts
  spr.setTextColor(C_DIM, C_BG); spr.drawString("mood", X, 50);
  uint8_t mood = statsMoodTier();
  for (int i = 0; i < 4; i++) {
    uint16_t c = i < mood ? (mood >= 3 ? C_GREEN : C_AMBER) : C_PANEL;
    spr.fillCircle(X + 56 + i * 14, 54, 4, c);
  }
  // fed — 10 pips (tokens toward next level)
  spr.setTextColor(C_DIM, C_BG); spr.drawString("fed", X, 68);
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++)
    spr.fillCircle(X + 40 + i * 12, 72, 3, i < fed ? C_GREEN : C_PANEL);
  // energy — 5 bars
  spr.setTextColor(C_DIM, C_BG); spr.drawString("energy", X, 86);
  uint8_t en = statsEnergyTier();
  for (int i = 0; i < 5; i++) {
    uint16_t c = i < en ? (en >= 4 ? C_GREEN : en >= 2 ? C_AMBER : C_RED) : C_PANEL;
    spr.fillRect(X + 56 + i * 14, 86, 10, 10, c);
  }
  // level + counters
  char b[40];
  spr.fillRoundRect(X, 106, 46, 16, 3, C_GREEN);
  spr.setTextColor(C_BG, C_GREEN); snprintf(b, sizeof(b), "Lv %u", stats().level);
  spr.drawString(b, X + 6, 110);
  spr.setTextColor(C_DIM, C_BG);
  snprintf(b, sizeof(b), "approved %u", stats().approvals); spr.drawString(b, X, 130);
  snprintf(b, sizeof(b), "denied   %u", stats().denials);   spr.drawString(b, X, 144);
  snprintf(b, sizeof(b), "tokens %lu", (unsigned long)stats().tokens); spr.drawString(b, X, 158);
}

// view 2 — plan usage: session + weekly gauges (from claude-usage v:1)
static void drawUsagePane() {
  const int X = PANE_X, W = SCREEN_W - X - 6;
  const UsageState& u = dataUsage();
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("USAGE", X, 32);
  if (!u.have) {
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("run the BLE", X, 56);
    spr.drawString("usage bridge", X, 70);
    return;
  }
  char b[32], rs[16];
  // session
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("session 5h", X, 50);
  spr.setTextColor(gaugeCol(u.sessPct), C_BG);
  snprintf(b, sizeof(b), "%d%%", u.sessPct); spr.drawString(b, X + W - 30, 50);
  drawBar(X, 64, W, 12, u.sessPct, gaugeCol(u.sessPct));
  fmtCountdown(u.sessResetS, rs, sizeof(rs));
  spr.setTextColor(C_DIM, C_BG); snprintf(b, sizeof(b), "resets %s", rs); spr.drawString(b, X, 80);
  // weekly
  spr.setTextColor(C_TEXT, C_BG); spr.drawString("weekly", X, 102);
  spr.setTextColor(gaugeCol(u.weekPct), C_BG);
  snprintf(b, sizeof(b), "%d%%", u.weekPct); spr.drawString(b, X + W - 30, 102);
  drawBar(X, 116, W, 12, u.weekPct, gaugeCol(u.weekPct));
  spr.setTextColor(C_DIM, C_BG);
  if (u.weekLabel[0]) snprintf(b, sizeof(b), "resets %s", u.weekLabel);
  else { fmtCountdown(u.weekResetS, rs, sizeof(rs)); snprintf(b, sizeof(b), "resets %s", rs); }
  spr.drawString(b, X, 132);
  char tk[16];
  if (u.todayTokens >= 1000) snprintf(tk, sizeof(tk), "%ld.%ldk", u.todayTokens/1000, (u.todayTokens%1000)/100);
  else snprintf(tk, sizeof(tk), "%ld", u.todayTokens);
  snprintf(b, sizeof(b), "today %s tok", tk); spr.drawString(b, X, 156);
}

// ── right pane dispatcher ──
static void drawDataPane() {
  spr.fillRect(PANE_X, 28, SCREEN_W - PANE_X, 188 - 28, C_BG);
  spr.setTextSize(1);
  if (paneView == 1)      drawPetStatsPane();
  else if (paneView == 2) drawUsagePane();
  else                    drawSessionsPane();
}

// ── footer (approval prompt OR stats strip) ──
static void drawFooter() {
  spr.fillRect(0, 216, SCREEN_W, SCREEN_H - 216, C_BG);
  spr.drawFastHLine(0, 216, SCREEN_W, C_PANEL);
  spr.setTextSize(1);
  bool inPrompt = tama.promptId[0] && !responseSent;
  if (inPrompt) {
    uint32_t waited = (millis() - promptArrivedMs) / 1000;
    spr.setTextColor((waited >= 10) ? C_RED : C_AMBER, C_BG);
    char b[48]; snprintf(b, sizeof(b), "approve: %.12s", tama.promptTool);
    spr.drawString(b, 6, 222);
    spr.setTextColor(C_GREEN, C_BG); spr.drawString("A=once", 180, 222);
    spr.setTextColor(C_RED, C_BG);   spr.drawString("C=deny", 250, 222);
  } else {
    char b[64];
    snprintf(b, sizeof(b), "appr %u  deny %u  Lv%u",
             stats().approvals, stats().denials, stats().level);
    spr.setTextColor(C_DIM, C_BG); spr.drawString(b, 6, 222);
  }
}

void setup() {
  Serial.begin(115200);
  platformBegin();
  C_BG    = tft.color565(12, 14, 18);   C_PANEL = tft.color565(22, 26, 32);
  C_TEXT  = tft.color565(235, 238, 245); C_DIM  = tft.color565(120, 128, 140);
  C_GREEN = tft.color565(60, 200, 110);  C_AMBER = tft.color565(240, 190, 60);
  C_RED   = tft.color565(235, 80, 70);
  // brief splash
  spr.fillSprite(C_BG);
  spr.setTextSize(2); spr.setTextColor(C_TEXT, C_BG);
  spr.drawString("Claude Buddy", 10, 100);
  spr.pushSprite(0, 0);
  statsLoad(); settingsLoad(); petNameLoad(); buddyInit();
  memset(&tama, 0, sizeof(tama));
  trInit("Claude Wio");                      // no-op stub under MOCK_DATA
#ifndef MOCK_DATA
  gifAvailable = characterInit(nullptr);     // host can push a GIF pack (opt-in BUDDY_GIF build)
  buddyMode = !gifAvailable;
#else
  dataSetDemo(true);
#endif
  spr.fillSprite(C_BG); spr.pushSprite(0, 0);
  Serial.println("HELLO");
}

void loop() {
  platformUpdate(); rtcTick();
  uint32_t now = millis();

  dataPoll(&tama);
  trLoop();                                  // no-op stub under MOCK_DATA
#ifdef MOCK_DATA
  // Demo mode animates session states but never raises a permission prompt.
  // Inject a synthetic one on a timer so the emulator exercises the approval
  // footer + attention state (demo's dataPoll leaves promptId untouched).
  if (((now / 1000) % 20) < 6) {
    strcpy(tama.promptId, "req_demo");
    strcpy(tama.promptTool, "Bash");
    strcpy(tama.promptHint, "git push origin main");
  } else {
    tama.promptId[0] = 0; tama.promptTool[0] = 0; tama.promptHint[0] = 0;
  }
#endif
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);

  // 1 Hz: tick the usage-gauge countdowns locally between bridge pushes
  static uint32_t lastSec = 0;
  if (now - lastSec >= 1000) { lastSec = now; dataUsageTick(); }

  // face-down → nap (refills energy; statsEnergyTier reads the nap timestamps)
  static int8_t faceDownFrames = 0; static bool napping = false; static uint32_t napStart = 0;
  bool down = platformFaceDown();
  if (down) { if (faceDownFrames < 20) faceDownFrames++; }
  else      { if (faceDownFrames > -10) faceDownFrames--; }
  if (!napping && faceDownFrames >= 15) { napping = true; napStart = now; }
  else if (napping && faceDownFrames <= -8) {
    napping = false; statsOnNapEnd((now - napStart) / 1000); statsOnWake();
  }

  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // attention LED (pulses while a prompt is pending)
  attentionLed(activeState == P_ATTENTION && settings().led && ((now / 400) % 2));

  // shake → dizzy
  static uint32_t lastShake = 0;
  if (now - lastShake > 50) {
    lastShake = now;
    if (platformShaken() && (int32_t)(now - oneShotUntil) >= 0) triggerOneShot(P_DIZZY, 2000);
  }

  // prompt arrival → chirp
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) { promptArrivedMs = now; beep(1200, 80); }
  }
  bool inPrompt = tama.promptId[0] && !responseSent;

  // ── buttons ──
  if (btnAPressed()) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd); responseSent = true;
      uint32_t took = (now - promptArrivedMs) / 1000; statsOnApproval(took);
      beep(2400, 60); if (took < 5) triggerOneShot(P_HEART, 2000);
    } else {
      buddyNextSpecies(); buddyInvalidate(); beep(1800, 30);
    }
  }
  if (btnCPressed() && inPrompt) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
    sendCmd(cmd); responseSent = true; statsOnDenial(); beep(600, 60);
  }
  if (btnBPressed()) { paneView = (paneView + 1) % PANE_COUNT; beep(2000, 20); }   // cycle right pane
  if (joyDown()) { msgScroll = (msgScroll >= 7) ? 0 : msgScroll + 1; beep(2000, 20); }
  if (joyUp() && msgScroll > 0) { msgScroll--; beep(2000, 20); }

  // ── render ──
  // Left pane: the buddy. On hardware a host-pushed GIF pack (from the FS) can
  // replace the ASCII buddy; the emulator has no filesystem (and the from-scratch
  // CPU interpreter can't run the AnimatedGIF decoder), so it always shows the
  // ASCII buddy — see README "Emulator vs hardware".
#ifdef MOCK_DATA
  buddyTick(activeState);
#else
  if (buddyMode) buddyTick(activeState);
  else { characterSetState(activeState); characterTick(); }
#endif

  // left-pane state label under the buddy
  spr.fillRect(0, 190, 150, 24, C_BG);
  spr.setTextSize(1); spr.setTextColor(C_DIM, C_BG);
  spr.drawString(STATE_NAMES[activeState], 8, 196);

  drawHeader();
  drawDataPane();
  drawFooter();
  spr.pushSprite(0, 0);

  delay(16);
}
