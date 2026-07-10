#if !defined(MOCK_DATA) && !defined(BUDDY_BLE)
// WiFi + SenseCraft OpenStream MQTT transport — see net_bridge.h for the
// architecture and measurement map.
//
// This file must stay its own translation unit: rpcWiFi drags in the rpc/STL
// headers that collide with Arduino's min()/max() macros if TFT_eSPI shares
// the TU (same constraint as ble_bridge.cpp).
//
// The wire-format logic (measurement accumulation, debounce, JSON-line
// synthesis, RX ring) lives in net_bridge_core.h — platform-free and shared
// byte-identical with the SenseCAP Indicator build (see the diff-guard note
// at the top of that file). This TU is only the Wio-specific shell: the
// rpcWiFi/PubSubClient connection state machine.
#include "net_bridge.h"
#include <Arduino.h>
#include <rpcWiFi.h>
#include <PubSubClient.h>
#include "net_bridge_core.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

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
  nbcOnPayload(topic, payload, length, millis());
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
      nbcTick(now);
      break;
  }
}

bool netConnected()  { return state == NS_RUN && mqtt.connected(); }
bool netSecure()     { return false; }
uint32_t netPasskey(){ return 0; }
void netClearBonds() {}

size_t netAvailable() { return nbcAvailable(); }
int    netRead()      { return nbcRead(); }
size_t netWrite(const uint8_t*, size_t) { return 0; }   // no uplink path
#endif // !MOCK_DATA && !BUDDY_BLE
