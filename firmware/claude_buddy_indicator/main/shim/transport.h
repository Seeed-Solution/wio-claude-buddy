#pragma once
// Indicator transport dispatch: the tr* surface data.h consumes, mapped
// onto the net_bridge_shell (WiFi + esp-mqtt around the shared
// net_bridge_core). Mirrors firmware/claude_buddy/transport.h's NET branch —
// same names, same semantics (trWrite is a no-op: the SenseCraft downlink
// has no uplink path).
#include <stdint.h>
#include <stddef.h>
#include "net_bridge_shell.h"

inline void     trInit(const char*) { nbsStart(); }
inline void     trLoop() { nbsLoop(); }
inline bool     trConnected() { return nbsConnected() != 0; }
inline bool     trSecure() { return false; }
inline uint32_t trPasskey() { return 0; }
inline void     trClearBonds() {}
inline size_t   trAvailable() { return nbsAvailable(); }
inline int      trRead() { return nbsRead(); }
inline size_t   trWrite(const uint8_t*, size_t) { return 0; }
