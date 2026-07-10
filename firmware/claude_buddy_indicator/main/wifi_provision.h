#pragma once
// On-device WiFi provisioning (design doc §WiFi provisioning):
//   boot -> load ≤5 NVS profiles -> try most-recently-successful (8 s each)
//        -> remaining known profiles -> give up and wait on the WiFi tab.
// The WiFi tab: async scan list (saved networks pinned on top), tap a
// secured network for the on-screen keyboard, Disconnect (session-only) and
// per-network Forget as distinct actions. First boot is seeded from
// indicator_secrets.h when it holds real credentials, so existing setups
// keep working; after that NVS is the only credential store.
//
// Owns the WiFi STA lifecycle (init/connect/disconnect). MQTT reacts via
// its own IP_EVENT handler in net_bridge_shell.c.
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void wpStart(void);                    // NVS + WiFi init + boot FSM kick-off
void wpLoop(uint32_t nowMs);           // poll: connect timeouts, UI refresh
void wpCreateTab(lv_obj_t* tab);       // build the WiFi tab UI
int  wpHasProfiles(void);              // 0 -> show the WiFi tab on boot
int  wpConnected(void);                // STA has an IP

#ifdef __cplusplus
}
#endif
