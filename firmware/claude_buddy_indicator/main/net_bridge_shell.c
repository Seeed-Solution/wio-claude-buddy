// WiFi + esp-mqtt shell around the shared net_bridge_core (see the .h for
// the single-owner-TU rule). This file is the Indicator's counterpart of
// the Wio's net_bridge.cpp: only the connection machinery lives here; the
// wire format is the shared core, byte-identical across targets.
//
// Also serves as the design doc's spikes 2 and 3 when flashed:
//   spike 2 — logs internal/PSRAM free heap before WiFi, after WiFi, and
//             after MQTT connect ("[heap]" lines);
//   spike 3 — logs every esp-mqtt DATA event's fragmentation geometry
//             ("[frag]" lines) and reassembles fragments per topic before
//             the core's strstr parser ever sees them.
#include "net_bridge_shell.h"

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#if __has_include("indicator_secrets.h")
#include "indicator_secrets.h"
#else
#include "indicator_secrets_example.h"
#endif

#include "net_bridge_core.h"   // sole owner TU of the core's static state

static const char* TAG = "net";

static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile int s_mqttUp = 0;
static char s_topicFilter[80];

static uint32_t nowMs(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void logHeap(const char* when) {
  ESP_LOGI(TAG, "[heap] %s: internal %u KB free, PSRAM %u KB free", when,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// ── esp-mqtt fragment reassembly ──
// OpenStream payloads are ~60 bytes, far below esp-mqtt's default 1024-byte
// buffer, so fragmentation should never trigger — but the core's strstr
// parser must NEVER see a partial JSON value, so reassembly is structural,
// not hopeful. event->topic is only present on the first fragment; the
// offsets stitch the rest.
#define R_TOPIC_CAP 128
#define R_DATA_CAP  512
static char    rTopic[R_TOPIC_CAP];
static uint8_t rData[R_DATA_CAP];
static int     rTotal = 0, rGot = 0, rDrop = 0;

static void onMqttData(esp_mqtt_event_handle_t ev) {
  if (ev->current_data_offset == 0) {
    rGot = 0;
    rTotal = ev->total_data_len;
    rDrop = (rTotal >= R_DATA_CAP) ||
            (ev->topic_len <= 0) || (ev->topic_len >= R_TOPIC_CAP);
    if (!rDrop) {
      memcpy(rTopic, ev->topic, ev->topic_len);
      rTopic[ev->topic_len] = 0;
    } else {
      ESP_LOGW(TAG, "[frag] dropping oversized publish (%d bytes)", rTotal);
    }
  }
  if (ev->total_data_len != ev->data_len) {   // actual fragmentation: log it
    ESP_LOGI(TAG, "[frag] offset=%d len=%d total=%d",
             ev->current_data_offset, ev->data_len, ev->total_data_len);
  }
  if (rDrop) return;
  if (ev->current_data_offset + ev->data_len > R_DATA_CAP) { rDrop = 1; return; }
  memcpy(rData + ev->current_data_offset, ev->data, ev->data_len);
  rGot = ev->current_data_offset + ev->data_len;
  if (rGot >= rTotal) {
    nbcOnPayload(rTopic, rData, (size_t)rTotal, nowMs());
  }
}

static void onMqttEvent(void* arg, esp_event_base_t base,
                        int32_t id, void* data) {
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_mqttUp = 1;
      esp_mqtt_client_subscribe(s_mqtt, s_topicFilter, 0);
      ESP_LOGI(TAG, "mqtt connected, subscribing %s", s_topicFilter);
      logHeap("mqtt up");
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_mqttUp = 0;
      ESP_LOGW(TAG, "mqtt disconnected (client auto-reconnects)");
      break;
    case MQTT_EVENT_DATA:
      onMqttData(ev);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "mqtt error type=%d", ev->error_handle->error_type);
      break;
    default:
      break;
  }
}

static void startMqtt(void) {
  if (s_mqtt) return;                        // already created; it reconnects
  static char user[32], clientId[48];
  snprintf(user, sizeof(user), "org-%s", SC_ORG_ID);
  snprintf(clientId, sizeof(clientId), "org-%s-ind%08lx",
           SC_ORG_ID, (unsigned long)esp_random());
  esp_mqtt_client_config_t cfg = {
    .broker.address.hostname = MQTT_HOST,
    .broker.address.port = MQTT_PORT,
    .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
    .credentials.username = user,
    .credentials.client_id = clientId,
    .credentials.authentication.password = SC_ACCESS_KEY,
    .session.keepalive = 30,
  };
  s_mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, onMqttEvent, NULL);
  esp_mqtt_client_start(s_mqtt);
  ESP_LOGI(TAG, "mqtt connecting to %s:%d as %s", MQTT_HOST, MQTT_PORT, clientId);
}

// WiFi is owned by wifi_provision.c (including the scan-to-list UI); this shell only reacts to the
// interface coming up (multiple handlers on IP_EVENT are fine).
static void onGotIp(void* arg, esp_event_base_t base, int32_t id, void* data) {
  ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
  ESP_LOGI(TAG, "wifi ok, ip " IPSTR, IP2STR(&e->ip_info.ip));
  logHeap("wifi up");
  startMqtt();
}

void nbsStart(void) {
  snprintf(s_topicFilter, sizeof(s_topicFilter),
           "/device_sensor_data/%s/%s/+/+/+", SC_ORG_ID, SC_DEVICE_EUI);
  logHeap("boot");
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             onGotIp, NULL));
}

void nbsLoop(void)      { nbcTick(nowMs()); }
int  nbsConnected(void) { return s_mqttUp; }
size_t nbsAvailable(void) { return nbcAvailable(); }
int    nbsRead(void)      { return nbcRead(); }

void nbsInjectPayload(const char* topic, const uint8_t* payload, size_t len) {
  nbcOnPayload(topic, payload, len, nowMs());
}
