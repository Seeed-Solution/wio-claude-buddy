// WiFi provisioning: NVS profile store + boot FSM + the WiFi tab UI.
// See wifi_provision.h for the design summary. All esp_wifi calls are
// thread-safe (they post to the wifi task); LVGL objects are only touched
// from the LVGL-locked sections (UI callbacks run inside the LVGL task,
// event-handler refreshes take the lv_port semaphore).
#include "wifi_provision.h"

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lv_port.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("indicator_secrets.h")
#include "indicator_secrets.h"
#else
#include "indicator_secrets_example.h"
#endif

static const char* TAG = "wifi";

// ── NVS profile store ──
#define MAX_PROFILES 5
typedef struct {
  char     ssid[33];
  char     pass[65];
  uint32_t rank;        // higher = more recently successful
} Profile;

static Profile  sProf[MAX_PROFILES];
static uint8_t  sNProf = 0;
static uint32_t sRankSeq = 0;

static void profilesLoad(void) {
  nvs_handle_t h;
  if (nvs_open("wifi_creds", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t n = 0;
  nvs_get_u8(h, "count", &n);
  nvs_get_u32(h, "rankseq", &sRankSeq);
  if (n > MAX_PROFILES) n = MAX_PROFILES;
  for (uint8_t i = 0; i < n; i++) {
    char key[8];
    size_t len = sizeof(sProf[i].ssid);
    snprintf(key, sizeof(key), "ssid%u", i);
    if (nvs_get_str(h, key, sProf[i].ssid, &len) != ESP_OK) continue;
    len = sizeof(sProf[i].pass);
    snprintf(key, sizeof(key), "pass%u", i);
    if (nvs_get_str(h, key, sProf[i].pass, &len) != ESP_OK) sProf[i].pass[0] = 0;
    snprintf(key, sizeof(key), "rank%u", i);
    nvs_get_u32(h, key, &sProf[i].rank);
    sNProf++;
  }
  nvs_close(h);
}

static void profilesSave(void) {
  nvs_handle_t h;
  if (nvs_open("wifi_creds", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "count", sNProf);
  nvs_set_u32(h, "rankseq", sRankSeq);
  for (uint8_t i = 0; i < sNProf; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ssid%u", i);
    nvs_set_str(h, key, sProf[i].ssid);
    snprintf(key, sizeof(key), "pass%u", i);
    nvs_set_str(h, key, sProf[i].pass);
    snprintf(key, sizeof(key), "rank%u", i);
    nvs_set_u32(h, key, sProf[i].rank);
  }
  nvs_commit(h);   // called from the LVGL task on save; commit is ~ms on
  nvs_close(h);    // this part (measured fine; move to a task if it grows)
}

static int profileFind(const char* ssid) {
  for (uint8_t i = 0; i < sNProf; i++)
    if (strcmp(sProf[i].ssid, ssid) == 0) return i;
  return -1;
}

static void profileUpsert(const char* ssid, const char* pass) {
  int i = profileFind(ssid);
  if (i < 0) {
    if (sNProf < MAX_PROFILES) {
      i = sNProf++;
    } else {              // evict the lowest-ranked (least recently OK)
      i = 0;
      for (uint8_t k = 1; k < MAX_PROFILES; k++)
        if (sProf[k].rank < sProf[i].rank) i = k;
    }
    strlcpy(sProf[i].ssid, ssid, sizeof(sProf[i].ssid));
  }
  strlcpy(sProf[i].pass, pass, sizeof(sProf[i].pass));
  profilesSave();
}

static void profileMarkOk(const char* ssid) {
  int i = profileFind(ssid);
  if (i < 0) return;
  sProf[i].rank = ++sRankSeq;
  profilesSave();
}

static void profileForget(const char* ssid) {
  int i = profileFind(ssid);
  if (i < 0) return;
  for (; i < sNProf - 1; i++) sProf[i] = sProf[i + 1];
  sNProf--;
  profilesSave();
}

// ── connection FSM ──
typedef enum { WP_IDLE, WP_TRYING, WP_ONLINE } WpState;
static volatile WpState sState = WP_IDLE;
static volatile int  sGotIp = 0, sDisconnected = 0, sScanDone = 0;
static char     sTrySsid[33];       // network currently being attempted
static uint32_t sTryDeadline = 0;
static int8_t   sTryOrder[MAX_PROFILES];
static int8_t   sTryIdx = -1;       // -1: not walking the known list
static int      sManualDisconnect = 0;

static void connectTo(const char* ssid, const char* pass, uint32_t nowMs) {
  wifi_config_t sta = { 0 };
  strlcpy((char*)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
  strlcpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
  strlcpy(sTrySsid, ssid, sizeof(sTrySsid));
  esp_wifi_disconnect();
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  esp_wifi_connect();
  sState = WP_TRYING;
  sTryDeadline = nowMs + 8000;
  sManualDisconnect = 0;
  ESP_LOGI(TAG, "joining '%s'...", ssid);
}

static void tryNextKnown(uint32_t nowMs) {
  if (sTryIdx < 0) return;
  if (sTryIdx >= sNProf) {
    sTryIdx = -1;
    sState = WP_IDLE;
    ESP_LOGW(TAG, "no known network reachable; waiting on the WiFi tab");
    return;
  }
  Profile* p = &sProf[sTryOrder[sTryIdx]];
  sTryIdx++;
  connectTo(p->ssid, p->pass, nowMs);
}

static void bootConnect(uint32_t nowMs) {
  if (sNProf == 0) { sState = WP_IDLE; return; }
  for (uint8_t i = 0; i < sNProf; i++) sTryOrder[i] = i;   // sort rank desc
  for (uint8_t a = 0; a < sNProf; a++)
    for (uint8_t b = a + 1; b < sNProf; b++)
      if (sProf[sTryOrder[b]].rank > sProf[sTryOrder[a]].rank) {
        int8_t t = sTryOrder[a]; sTryOrder[a] = sTryOrder[b]; sTryOrder[b] = t;
      }
  sTryIdx = 0;
  tryNextKnown(nowMs);
}

// ── scan ──
#define MAX_SCAN 16
static wifi_ap_record_t sScan[MAX_SCAN];
static uint16_t sNScan = 0;
static int sScanning = 0;

static void scanStart(void) {
  if (sScanning) return;
  if (esp_wifi_scan_start(NULL, false) == ESP_OK) {   // async
    sScanning = 1;
    ESP_LOGI(TAG, "scanning...");
  }
}

// ── WiFi events (esp_event task; no LVGL calls here) ──
static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t* e = (wifi_event_sta_disconnected_t*)data;
    ESP_LOGW(TAG, "'%s' disconnected (reason=%d)", sTrySsid, e->reason);
    sDisconnected = 1;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    sNScan = MAX_SCAN;
    if (esp_wifi_scan_get_ap_records(&sNScan, sScan) != ESP_OK) sNScan = 0;
    sScanning = 0;
    sScanDone = 1;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    sGotIp = 1;
  }
}

// ── UI ──
static lv_obj_t *sTab, *sConnLbl, *sList, *sKbModal;
static char sPendingSsid[33];

static void listRefresh(void);

static void onDisconnectBtn(lv_event_t* e) {
  (void)e;
  sManualDisconnect = 1;               // stay down until the user reconnects
  sTryIdx = -1;
  esp_wifi_disconnect();
  ESP_LOGI(TAG, "manual disconnect (credentials kept)");
}

static void onScanBtn(lv_event_t* e) { (void)e; scanStart(); }

static void kbClose(void) {
  if (sKbModal) { lv_obj_del(sKbModal); sKbModal = NULL; }
}

static void onKbEvent(lv_event_t* e) {
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_READY) {
    const char* pass = lv_textarea_get_text(ta);
    profileUpsert(sPendingSsid, pass);
    connectTo(sPendingSsid, pass, lv_tick_get());
    kbClose();
  } else if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
    kbClose();
  }
}

static void kbOpen(const char* ssid) {
  strlcpy(sPendingSsid, ssid, sizeof(sPendingSsid));
  sKbModal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(sKbModal, 480, 480);
  lv_obj_set_style_bg_color(sKbModal, lv_color_hex(0x121417), 0);
  lv_obj_set_style_bg_opa(sKbModal, LV_OPA_90, 0);

  lv_obj_t* lbl = lv_label_create(sKbModal);
  lv_label_set_text_fmt(lbl, "password for %s", ssid);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t* ta = lv_textarea_create(sKbModal);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, true);
  lv_obj_set_width(ta, 400);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 52);

  lv_obj_t* kb = lv_keyboard_create(sKbModal);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_set_size(kb, 456, 220);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_event_cb(kb, onKbEvent, LV_EVENT_READY, ta);
  lv_obj_add_event_cb(kb, onKbEvent, LV_EVENT_CANCEL, ta);
}

static void onSavedRow(lv_event_t* e) {
  const char* ssid = (const char*)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    profileForget(ssid);
    ESP_LOGI(TAG, "forgot '%s'", ssid);
    listRefresh();
  } else if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) {
    int i = profileFind(ssid);
    if (i >= 0) connectTo(sProf[i].ssid, sProf[i].pass, lv_tick_get());
  }
}

static void onScanRow(lv_event_t* e) {
  wifi_ap_record_t* ap = (wifi_ap_record_t*)lv_event_get_user_data(e);
  const char* ssid = (const char*)ap->ssid;
  int known = profileFind(ssid);
  if (known >= 0) {
    connectTo(sProf[known].ssid, sProf[known].pass, lv_tick_get());
  } else if (ap->authmode == WIFI_AUTH_OPEN) {
    profileUpsert(ssid, "");
    connectTo(ssid, "", lv_tick_get());
  } else {
    kbOpen(ssid);
  }
}

static void listRefresh(void) {
  if (!sList) return;
  lv_obj_clean(sList);
  for (uint8_t i = 0; i < sNProf; i++) {   // saved first (star; hold to forget)
    char txt[64];
    snprintf(txt, sizeof(txt), "%s  (saved - hold to forget)", sProf[i].ssid);
    lv_obj_t* b = lv_list_add_btn(sList, LV_SYMBOL_SAVE, txt);
    lv_obj_add_event_cb(b, onSavedRow, LV_EVENT_SHORT_CLICKED, sProf[i].ssid);
    lv_obj_add_event_cb(b, onSavedRow, LV_EVENT_LONG_PRESSED, sProf[i].ssid);
  }
  for (uint16_t i = 0; i < sNScan; i++) {
    const char* ssid = (const char*)sScan[i].ssid;
    if (!ssid[0] || profileFind(ssid) >= 0) continue;   // dedup vs saved
    int dup = 0;                                        // dedup within scan
    for (uint16_t k = 0; k < i; k++)
      if (strcmp((const char*)sScan[k].ssid, ssid) == 0) { dup = 1; break; }
    if (dup) continue;
    char txt[64];
    snprintf(txt, sizeof(txt), "%s  (%d dBm)", ssid, sScan[i].rssi);
    lv_obj_t* b = lv_list_add_btn(
        sList, sScan[i].authmode == WIFI_AUTH_OPEN ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE,
        txt);
    lv_obj_add_event_cb(b, onScanRow, LV_EVENT_SHORT_CLICKED, &sScan[i]);
  }
}

void wpCreateTab(lv_obj_t* tab) {
  sTab = tab;
  sConnLbl = lv_label_create(tab);
  lv_label_set_text(sConnLbl, "not connected");
  lv_obj_set_style_text_color(sConnLbl, lv_color_white(), 0);
  lv_obj_align(sConnLbl, LV_ALIGN_TOP_LEFT, 12, 8);

  lv_obj_t* scanBtn = lv_btn_create(tab);
  lv_obj_align(scanBtn, LV_ALIGN_TOP_RIGHT, -12, 4);
  lv_obj_add_event_cb(scanBtn, onScanBtn, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l1 = lv_label_create(scanBtn);
  lv_label_set_text(l1, LV_SYMBOL_REFRESH " Scan");

  lv_obj_t* dcBtn = lv_btn_create(tab);
  lv_obj_set_style_bg_color(dcBtn, lv_color_hex(0x5f2120), 0);
  lv_obj_align(dcBtn, LV_ALIGN_TOP_RIGHT, -130, 4);
  lv_obj_add_event_cb(dcBtn, onDisconnectBtn, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l2 = lv_label_create(dcBtn);
  lv_label_set_text(l2, "Disconnect");

  sList = lv_list_create(tab);
  lv_obj_set_size(sList, 440, 300);
  lv_obj_align(sList, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_color(sList, lv_color_hex(0x1c1f24), 0);
  listRefresh();
}

// ── public lifecycle ──
void wpStart(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             onWifiEvent, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             onWifiEvent, NULL));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  profilesLoad();
  // One-time migration: seed NVS from indicator_secrets.h so pre-provisioning
  // setups keep working (skipped once any profile exists or for placeholders).
  if (sNProf == 0 && strcmp(WIFI_SSID, "your-ssid") != 0) {
    profileUpsert(WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "seeded NVS with '%s' from indicator_secrets.h", WIFI_SSID);
  }
  bootConnect(lv_tick_get());
}

void wpLoop(uint32_t nowMs) {
  if (sGotIp) {
    sGotIp = 0;
    sState = WP_ONLINE;
    sTryIdx = -1;
    profileMarkOk(sTrySsid);
    ESP_LOGI(TAG, "'%s' online", sTrySsid);
  }
  if (sDisconnected) {
    sDisconnected = 0;
    if (sManualDisconnect) {
      sState = WP_IDLE;                        // user asked; stay down
    } else if (sTryIdx >= 0) {
      tryNextKnown(nowMs);                     // walking the known list
    } else if (sState == WP_ONLINE || sState == WP_TRYING) {
      esp_wifi_connect();                      // drop/bad password: retry
      sState = WP_TRYING;
      sTryDeadline = nowMs + 8000;
    }
  }
  if (sState == WP_TRYING && (int32_t)(nowMs - sTryDeadline) >= 0) {
    if (sTryIdx >= 0) tryNextKnown(nowMs);
    else { esp_wifi_disconnect(); sState = WP_IDLE; }
  }
  if (sScanDone) {                             // refresh list under LVGL lock
    sScanDone = 0;
    listRefresh();                             // called from main loop, which
  }                                            // holds the lv_port semaphore
  static uint32_t lastLbl = 0;                 // connection label @1 Hz
  if (sConnLbl && nowMs - lastLbl >= 1000) {
    lastLbl = nowMs;
    if (sState == WP_ONLINE) {
      esp_netif_ip_info_t ip;
      esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK)
        lv_label_set_text_fmt(sConnLbl, LV_SYMBOL_WIFI " %s  " IPSTR,
                              sTrySsid, IP2STR(&ip.ip));
    } else if (sState == WP_TRYING) {
      lv_label_set_text_fmt(sConnLbl, "joining %s ...", sTrySsid);
    } else {
      lv_label_set_text(sConnLbl, "not connected - scan to choose a network");
    }
  }
}

int wpHasProfiles(void) { return sNProf > 0; }
int wpConnected(void)   { return sState == WP_ONLINE; }
