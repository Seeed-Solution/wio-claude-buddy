#pragma once
// Indicator stand-in for the Wio's platform header, satisfying what the
// shared headers (data.h) include by name. Same soft-RTC surface, backed by
// esp_timer instead of millis()-advanced SAMD structs. Phase 2 grows this
// into the full indicator_platform (LVGL display/touch) — the RTC part
// stays here so data.h keeps compiling verbatim.
#include <stdint.h>
#include "esp_timer.h"

struct RTC_TimeTypeDef { uint8_t Hours, Minutes, Seconds; };
struct RTC_DateTypeDef { uint8_t WeekDay, Month, Date; uint16_t Year; };

// Soft RTC: synced once by data.h's {"time":[...]} handler, advanced from
// the monotonic clock on read.
static RTC_TimeTypeDef _rtcTime = {0, 0, 0};
static RTC_DateTypeDef _rtcDate = {0, 1, 1, 2000};
static uint64_t _rtcSyncUs = 0;

static inline void rtcSetTime(const RTC_TimeTypeDef* t) {
  _rtcTime = *t;
  _rtcSyncUs = (uint64_t)esp_timer_get_time();
}
static inline void rtcSetDate(const RTC_DateTypeDef* d) { _rtcDate = *d; }

static inline void rtcGetDate(RTC_DateTypeDef* d) { *d = _rtcDate; }
static inline void rtcGetTime(RTC_TimeTypeDef* t) {
  uint32_t elapsed = (uint32_t)(((uint64_t)esp_timer_get_time() - _rtcSyncUs) / 1000000ULL);
  uint32_t s = _rtcTime.Seconds + elapsed;
  t->Seconds = (uint8_t)(s % 60);
  uint32_t m = _rtcTime.Minutes + s / 60;
  t->Minutes = (uint8_t)(m % 60);
  t->Hours   = (uint8_t)((_rtcTime.Hours + m / 60) % 24);
  // date rollover is Phase 2's real-RTC concern; the panes only show H:M
}
