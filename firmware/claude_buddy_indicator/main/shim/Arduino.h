#pragma once
// Minimal Arduino environment for the verbatim-shared Wio headers (data.h,
// xfer.h, stats.h, prefs_compat.h) under ESP-IDF. This is an include-path
// shim, NOT arduino-esp32: main/ lists shim/ first, so the shared headers'
// `#include <Arduino.h>` lands here and their text never changes (the same
// trick keeps wio_platform.h / transport.h verbatim — see those shims).
// Only what the shared headers actually use lives here; grow it reluctantly.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"

static inline uint32_t millis(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Arduino-style min/max for stats.h — as templates, NOT macros: macro
// min/max blow up the libstdc++ headers ArduinoJson includes under IDF's
// gnu++2b (the same min()/max() collision the Wio build dodges by keeping
// rpc headers out of TFT_eSPI TUs).
template <typename T, typename U>
static inline auto min(T a, U b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
template <typename T, typename U>
static inline auto max(T a, U b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}

// Just enough Stream for data.h::_LineBuf::feed(Serial, out). The Indicator
// has no USB-serial JSON path (the SenseCraft downlink is the only feed), so
// Serial is a permanently-empty stream.
class Stream {
 public:
  int available() { return 0; }
  int read() { return -1; }
};
static Stream Serial;
