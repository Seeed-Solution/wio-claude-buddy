#pragma once
#include <stdint.h>
#include <stddef.h>

// WiFi + SenseCraft OpenStream MQTT bridge (default transport).
//
// The host bridge (host/buddy_sensecraft_bridge.py) uplinks numeric
// measurements to the SenseCraft Data Platform; the platform's OpenStream
// broker pushes them back out per-measurement:
//
//   topic:   /device_sensor_data/<orgID>/<deviceEUI>/<channel>/<rsvd>/<measID>
//   payload: {"value":<number>,"timestamp":<ms>}
//
// net_bridge accumulates those and SYNTHESIZES the original newline-JSON
// lines (heartbeat / usage v:1 / time sync) into the same RX ring buffer the
// BLE transport used, so data.h::_applyJson is untouched.
//
// Measurement map (channel "1"; must match buddy_sensecraft_bridge.py — the
// platform silently drops custom/unknown measurement IDs, so standard IDs
// are reused):
//   4097 total          4102 session.pct      4106 today.tokens
//   4098 running        4103 session.reset_s  4108 tz_offset_sec + 86400
//   4099 waiting        4104 week.pct              (biased so the wire never
//   4100 tokens         4105 week.reset_s           carries a negative)
//   4101 tokens_today
//
// The soft RTC is synced from any payload's timestamp (host wall clock, ms)
// plus the un-biased 4108 offset — no NTP needed.
//
// Unlike rpcBLE (which cannot re-advertise after a disconnect), this
// transport reconnects WiFi and MQTT automatically — no replug needed.

void     netInit(const char* deviceName);   // name unused; kept for tr* parity
void     netLoop();                         // drives the WiFi/MQTT state machine
bool     netConnected();
bool     netSecure();                       // false — plain MQTT :1883
uint32_t netPasskey();                      // 0 — no pairing concept
void     netClearBonds();                   // no-op
size_t   netAvailable();
int      netRead();
size_t   netWrite(const uint8_t* data, size_t len);   // no uplink — returns 0
