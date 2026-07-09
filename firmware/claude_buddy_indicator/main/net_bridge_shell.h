#pragma once
// Indicator network shell: WiFi STA + esp-mqtt subscribe to the SenseCraft
// OpenStream broker, feeding the shared net_bridge_core (which this .c file
// is the single owner of — the core header carries static state and must
// live in exactly one translation unit, same rule as net_bridge.cpp on the
// Wio). Everything else reaches the core through these functions.
//
// Phase 2 replaces the compile-time indicator_secrets.h WiFi credentials
// with the NVS-backed provisioning UI; the MQTT/core path stays.
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   nbsStart(void);      // NVS init + WiFi join + MQTT subscribe (async)
void   nbsLoop(void);       // core debounce/flush tick — call every loop pass
int    nbsConnected(void);  // MQTT session up
size_t nbsAvailable(void);  // synthesized-JSON ring (data.h's tr* feed)
int    nbsRead(void);

// Test hook: feed one payload as if the broker delivered it (used by the
// boot self-test to prove the core+data.h path before any network exists).
void   nbsInjectPayload(const char* topic, const uint8_t* payload, size_t len);

#ifdef __cplusplus
}
#endif
