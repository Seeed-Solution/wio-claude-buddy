#pragma once
// Indicator transport dispatch: the tr* surface data.h consumes, mapped
// straight onto the shared net_bridge_core ring (the esp-mqtt shell feeds
// nbcOnPayload; Phase 2's net_bridge_shell.c owns the connection). Mirrors
// firmware/claude_buddy/transport.h's NET branch — same names, same
// semantics (trWrite is a no-op: the SenseCraft downlink has no uplink path).
#include <stdint.h>
#include <stddef.h>
#include "net_bridge_core.h"

inline void     trInit(const char*) {}
inline void     trLoop() {}
inline bool     trConnected() { return true; }   // Phase 2: esp-mqtt state
inline bool     trSecure() { return false; }
inline uint32_t trPasskey() { return 0; }
inline void     trClearBonds() {}
inline size_t   trAvailable() { return nbcAvailable(); }
inline int      trRead() { return nbcRead(); }
inline size_t   trWrite(const uint8_t*, size_t) { return 0; }
