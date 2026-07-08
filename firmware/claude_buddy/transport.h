#pragma once
#include <stdint.h>
#include <stddef.h>

// Transport dispatch. The pet/data code talks to one byte-stream link through
// these tr* functions; which radio backs them is a compile-time choice (the
// RTL8720 coprocessor runs ONE of rpcBLE/rpcWiFi per build, never both).
//
//   default      → ble_bridge (NUS peripheral: Claude Desktop / BLE bridge)
//   -DMOCK_DATA  → inline no-op stubs (emulator/demo build, no radio)
//
// The backend headers included here are declaration-only, so transport.h is
// safe to include from the same translation unit as TFT_eSPI.h (the rpc STL
// headers are not — keep those confined to the *_bridge.cpp files).

#if defined(MOCK_DATA)

inline void     trInit(const char*) {}
inline void     trLoop() {}
inline bool     trConnected() { return false; }
inline bool     trSecure() { return false; }
inline uint32_t trPasskey() { return 0; }
inline void     trClearBonds() {}
inline size_t   trAvailable() { return 0; }
inline int      trRead() { return -1; }
inline size_t   trWrite(const uint8_t*, size_t) { return 0; }

#else  // BLE

#include "ble_bridge.h"
inline void     trInit(const char* name) { bleInit(name); }
inline void     trLoop() { bleLoop(); }
inline bool     trConnected() { return bleConnected(); }
inline bool     trSecure() { return bleSecure(); }
inline uint32_t trPasskey() { return blePasskey(); }
inline void     trClearBonds() { bleClearBonds(); }
inline size_t   trAvailable() { return bleAvailable(); }
inline int      trRead() { return bleRead(); }
inline size_t   trWrite(const uint8_t* d, size_t n) { return bleWrite(d, n); }

#endif
