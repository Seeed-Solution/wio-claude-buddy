#if !defined(MOCK_DATA) && !defined(BUDDY_BLE)
// WiFi + SenseCraft OpenStream MQTT transport — see net_bridge.h for the
// architecture and measurement map.
//
// This file must stay its own translation unit: rpcWiFi drags in the rpc/STL
// headers that collide with Arduino's min()/max() macros if TFT_eSPI shares
// the TU (same constraint as ble_bridge.cpp). It also deliberately avoids
// ArduinoJson — the downlink payload is a tiny fixed shape, parsed with
// strstr/strtod, keeping heap churn and header risk out of this TU.
#include "net_bridge.h"
#include <Arduino.h>
#include <rpcWiFi.h>
#include <PubSubClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

// ── RX ring buffer (same pattern as ble_bridge.cpp; drained by dataPoll) ──
static const size_t RX_CAP = 2048;
static uint8_t rxBuf[RX_CAP];
static volatile size_t rxHead = 0, rxTail = 0;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t nx = (rxHead + 1) % RX_CAP;
    if (nx == rxTail) return;      // full — drop (reader should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = nx;
  }
}

static void rxPushLine(const char* s) {
  rxPush((const uint8_t*)s, strlen(s));
  rxPush((const uint8_t*)"\n", 1);
}

// ── measurement accumulator ──
// Index = measurement ID - 4097 (4108 folds to index 10; 4107 unused).
static const int N_MEAS = 11;
static const uint32_t TZ_BIAS = 86400;
static double   meas[N_MEAS] = {0};
static bool     seen[N_MEAS] = {false};
static uint64_t lastTsMs = 0;         // host wall-clock ms from any payload

// Two flush groups: heartbeat (idx 0-4) and usage (idx 5-9). tz (idx 10)
// rides along with the time-sync line.
struct Group { uint32_t lastRxMs; uint32_t dirtySinceMs; bool dirty; };
static Group gHb = {0, 0, false}, gUs = {0, 0, false};
static const uint32_t FLUSH_QUIET_MS = 500;    // flush after the burst settles
static const uint32_t FLUSH_MAX_MS   = 2500;   // ...but never later than this
static uint32_t lastTimeSyncMs = 0;            // re-sync soft RTC every 10 min

static int measIndex(long id) {
  if (id >= 4097 && id <= 4106) return (int)(id - 4097);
  if (id == 4108) return 10;
  return -1;
}

// ── connection state machine ──
enum NetState { NS_WIFI_BEGIN, NS_WIFI_WAIT, NS_MQTT, NS_RUN };
static NetState  state = NS_WIFI_BEGIN;
static uint32_t  stateMs = 0;         // entry time of current state
static uint32_t  nextWifiTryMs = 0;   // 0 → join immediately
static uint32_t  mqttBackoffMs = 5000;
static uint32_t  nextMqttTryMs = 0;
static WiFiClient    wifiClient;
static PubSubClient  mqtt(wifiClient);
static char topicFilter[80];
static char clientId[48];

static void onMessage(char* topic, byte* payload, unsigned int length) {
  // measurement ID = last topic segment; channel (3rd segment) is ignored —
  // the host uplinks everything on channel 1.
  const char* seg = strrchr(topic, '/');
  if (!seg) return;
  long id = strtol(seg + 1, nullptr, 10);
  int idx = measIndex(id);
  if (idx < 0) return;

  char body[96];
  if (length >= sizeof(body)) return;      // not ours — payloads are tiny
  memcpy(body, payload, length);
  body[length] = 0;

  const char* v = strstr(body, "\"value\":");
  if (!v) return;
  v += 8;
  if (*v == '"') v++;                      // tolerate quoted numbers
  meas[idx] = strtod(v, nullptr);
  seen[idx] = true;
  const char* t = strstr(body, "\"timestamp\":");
  if (t) {
    t += 12;
    if (*t == '"') t++;                    // broker sends it as a quoted string
    uint64_t ts = strtoull(t, nullptr, 10);
    if (ts > 1000000000000ULL) lastTsMs = ts;
  }

  uint32_t now = millis();
  Group* g = (idx <= 4) ? &gHb : (idx <= 9) ? &gUs : nullptr;
  if (g) {
    g->lastRxMs = now;
    if (!g->dirty) { g->dirty = true; g->dirtySinceMs = now; }
  }
}

// ── JSON synthesis (the same lines the BLE bridge used to send) ──
static void flushTimeSync(uint32_t now) {
  if (!seen[10] || lastTsMs == 0) return;
  if (lastTimeSyncMs != 0 && now - lastTimeSyncMs < 600000UL) return;
  long off = (long)meas[10] - (long)TZ_BIAS;
  char line[64];
  snprintf(line, sizeof(line), "{\"time\":[%lu,%ld]}",
           (unsigned long)(lastTsMs / 1000ULL), off);
  rxPushLine(line);
  lastTimeSyncMs = now;
}

static void flushHeartbeat() {
  long total   = (long)meas[0], running = (long)meas[1], waiting = (long)meas[2];
  long tokens  = (long)meas[3], tokToday = (long)meas[4];
  char msg[24];
  if (running > 0) snprintf(msg, sizeof(msg), "%ld active", running);
  else             snprintf(msg, sizeof(msg), "idle");
  char line[160];
  snprintf(line, sizeof(line),
           "{\"total\":%ld,\"running\":%ld,\"waiting\":%ld,\"msg\":\"%s\","
           "\"tokens\":%ld,\"tokens_today\":%ld}",
           total, running, waiting, msg, tokens, tokToday);
  rxPushLine(line);
}

static void flushUsage() {
  char line[160];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"session\":{\"pct\":%ld,\"reset_s\":%ld},"
           "\"week\":{\"pct\":%ld,\"reset_s\":%ld},\"today\":{\"tokens\":%ld}}",
           (long)meas[5], (long)meas[6], (long)meas[7], (long)meas[8], (long)meas[9]);
  rxPushLine(line);
}

static bool groupDue(const Group& g, uint32_t now) {
  return g.dirty && (now - g.lastRxMs >= FLUSH_QUIET_MS ||
                     now - g.dirtySinceMs >= FLUSH_MAX_MS);
}

// ── public API ──
void netInit(const char*) {
  snprintf(topicFilter, sizeof(topicFilter),
           "/device_sensor_data/%s/%s/+/+/+", SC_ORG_ID, SC_DEVICE_EUI);
  snprintf(clientId, sizeof(clientId), "org-%s-wio%08lx",
           SC_ORG_ID, (unsigned long)micros());
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setSocketTimeout(3);            // connect() blocks up to this per try
  mqtt.setKeepAlive(30);
  state = NS_WIFI_BEGIN;
  stateMs = millis();
  Serial.printf("[net] transport: WiFi + MQTT %s:%d\n", MQTT_HOST, (int)MQTT_PORT);
}

void netLoop() {
  uint32_t now = millis();
  switch (state) {
    case NS_WIFI_BEGIN:
      if (nextWifiTryMs == 0 || (int32_t)(now - nextWifiTryMs) >= 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.printf("[net] wifi joining '%s'...\n", WIFI_SSID);
        state = NS_WIFI_WAIT;
        stateMs = now;
      }
      break;

    case NS_WIFI_WAIT:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[net] wifi ok, ip %s\n", WiFi.localIP().toString().c_str());
        state = NS_MQTT;
        stateMs = now;
        nextMqttTryMs = now;           // try immediately
        mqttBackoffMs = 5000;
      } else if (now - stateMs >= 15000) {
        Serial.println("[net] wifi timeout; retrying in 5s");
        state = NS_WIFI_BEGIN;
        nextWifiTryMs = now + 5000;
      }
      break;

    case NS_MQTT:
      if (WiFi.status() != WL_CONNECTED) { state = NS_WIFI_BEGIN; stateMs = now; break; }
      if ((int32_t)(now - nextMqttTryMs) >= 0) {
        Serial.printf("[net] mqtt connecting to %s as %s...\n", MQTT_HOST, clientId);
        char user[32];
        snprintf(user, sizeof(user), "org-%s", SC_ORG_ID);
        if (mqtt.connect(clientId, user, SC_ACCESS_KEY)) {
          mqtt.subscribe(topicFilter);
          Serial.printf("[net] mqtt subscribed %s\n", topicFilter);
          state = NS_RUN;
        } else {
          Serial.printf("[net] mqtt failed rc=%d; retry in %lus\n",
                        mqtt.state(), (unsigned long)(mqttBackoffMs / 1000));
          nextMqttTryMs = now + mqttBackoffMs;
          if (mqttBackoffMs < 60000) mqttBackoffMs *= 2;
        }
      }
      break;

    case NS_RUN:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[net] wifi lost; rejoining");
        state = NS_WIFI_BEGIN;
        stateMs = now;
        break;
      }
      if (!mqtt.connected()) {
        Serial.println("[net] mqtt lost; reconnecting");
        state = NS_MQTT;
        nextMqttTryMs = now;
        mqttBackoffMs = 5000;
        break;
      }
      mqtt.loop();
      if (groupDue(gHb, now)) { gHb.dirty = false; flushHeartbeat(); flushTimeSync(now); }
      if (groupDue(gUs, now)) { gUs.dirty = false; flushUsage(); }
      break;
  }
}

bool netConnected()  { return state == NS_RUN && mqtt.connected(); }
bool netSecure()     { return false; }
uint32_t netPasskey(){ return 0; }
void netClearBonds() {}

size_t netAvailable() { return (rxHead + RX_CAP - rxTail) % RX_CAP; }
int netRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}
size_t netWrite(const uint8_t*, size_t) { return 0; }   // no uplink path
#endif // !MOCK_DATA && !BUDDY_BLE
