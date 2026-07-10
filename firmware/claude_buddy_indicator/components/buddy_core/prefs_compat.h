#pragma once
#include <Arduino.h>
#include <string.h>

// A drop-in subset of ESP32 Preferences, backed by a single fixed-layout
// struct. stats.h uses a known, fixed set of keys (namespace "buddy"); we
// map each to a field. Unknown keys return the provided default and are
// ignored on write (keeps the port total).
//
// Storage model: the blob is a single process-wide RAM cache (gBlob() —
// one instance shared across every translation unit that includes this
// header, since function-local statics in an inline function are unified).
// On hardware it is lazily loaded from, and flushed to, FlashStorage_SAMD.
// Under MOCK_DATA (the web emulator) all flash access is compiled out — the
// emulator cannot safely erase/write NVM — so the blob is RAM-only and
// resets each boot, which is fine for the demo.

struct _BuddyBlob {
  uint32_t magic;          // _BUDDY_MAGIC once initialized
  // stats
  uint32_t nap;            // "nap"
  uint16_t appr;           // "appr"
  uint16_t deny;           // "deny"
  uint8_t  vidx, vcnt, lvl;// "vidx","vcnt","lvl"
  uint32_t tok;            // "tok"
  uint16_t vel[8];         // "vel" (getBytes/putBytes)
  // settings
  uint8_t  s_snd, s_bt, s_wifi, s_led, s_hud, s_crot;
  // names + species
  char     petname[24];    // "petname"
  char     owner[32];      // "owner"
  uint8_t  species;        // "species"
};

static const uint32_t _BUDDY_MAGIC = 0xB0DD0001;

// RAM-only storage. We do NOT persist to flash on hardware: a FlashStorage NVM
// erase/write takes several ms and wedges the rpcBLE/RTL8720 link, hard-crashing
// the board (it happened on every button press that saved stats/species). Stats,
// settings, owner and species therefore reset on reboot — an acceptable trade
// for not crashing. (The host re-sends owner on connect anyway.)

inline void _blobDefaults(_BuddyBlob& b) {
  memset(&b, 0, sizeof(b));
  b.magic = _BUDDY_MAGIC;
  b.s_snd = 1; b.s_bt = 1; b.s_led = 1; b.s_hud = 1;
  strncpy(b.petname, "Buddy", sizeof(b.petname));
  b.species = 0xFF;
}

// The single shared RAM cache (initialized to defaults each boot).
inline _BuddyBlob& gBlob() {
  static _BuddyBlob b;
  static bool init = false;
  if (!init) { init = true; _blobDefaults(b); }
  return b;
}

inline void _blobFlush() {
  // no-op: RAM-only (see note above — flash writes crash BLE).
}

class Preferences {
  bool _ro = true, _dirty = false;
 public:
  bool begin(const char* /*ns*/, bool readOnly = false) {
    gBlob();                       // ensure loaded/initialized
    _ro = readOnly; _dirty = false; return true;
  }
  void end() { if (_dirty && !_ro) _blobFlush(); _dirty = false; }
  void clear() { _blobDefaults(gBlob()); _dirty = true; }

  uint32_t getUInt(const char* k, uint32_t d = 0) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"nap")) return b.nap;
    if (!strcmp(k,"tok")) return b.tok;
    return d;
  }
  void putUInt(const char* k, uint32_t v) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"nap")) b.nap = v; else if (!strcmp(k,"tok")) b.tok = v; else return;
    _dirty = true;
  }
  uint16_t getUShort(const char* k, uint16_t d = 0) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"appr")) return b.appr;
    if (!strcmp(k,"deny")) return b.deny;
    return d;
  }
  void putUShort(const char* k, uint16_t v) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"appr")) b.appr = v; else if (!strcmp(k,"deny")) b.deny = v; else return;
    _dirty = true;
  }
  uint8_t getUChar(const char* k, uint8_t d = 0) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"vidx")) return b.vidx;
    if (!strcmp(k,"vcnt")) return b.vcnt;
    if (!strcmp(k,"lvl"))  return b.lvl;
    if (!strcmp(k,"s_crot")) return b.s_crot;
    if (!strcmp(k,"species")) return b.species;
    return d;
  }
  void putUChar(const char* k, uint8_t v) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"vidx")) b.vidx = v;
    else if (!strcmp(k,"vcnt")) b.vcnt = v;
    else if (!strcmp(k,"lvl")) b.lvl = v;
    else if (!strcmp(k,"s_crot")) b.s_crot = v;
    else if (!strcmp(k,"species")) b.species = v;
    else return;
    _dirty = true;
  }
  bool getBool(const char* k, bool d = false) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"s_snd")) return b.s_snd;
    if (!strcmp(k,"s_bt"))  return b.s_bt;
    if (!strcmp(k,"s_wifi"))return b.s_wifi;
    if (!strcmp(k,"s_led")) return b.s_led;
    if (!strcmp(k,"s_hud")) return b.s_hud;
    return d;
  }
  void putBool(const char* k, bool v) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"s_snd")) b.s_snd = v;
    else if (!strcmp(k,"s_bt")) b.s_bt = v;
    else if (!strcmp(k,"s_wifi")) b.s_wifi = v;
    else if (!strcmp(k,"s_led")) b.s_led = v;
    else if (!strcmp(k,"s_hud")) b.s_hud = v;
    else return;
    _dirty = true;
  }
  size_t getString(const char* k, char* buf, size_t len) {
    _BuddyBlob& b = gBlob();
    const char* src = !strcmp(k,"petname") ? b.petname : !strcmp(k,"owner") ? b.owner : "";
    strncpy(buf, src, len); buf[len-1] = 0; return strlen(buf);
  }
  void putString(const char* k, const char* v) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"petname")) { strncpy(b.petname, v, sizeof(b.petname)); b.petname[sizeof(b.petname)-1]=0; }
    else if (!strcmp(k,"owner")) { strncpy(b.owner, v, sizeof(b.owner)); b.owner[sizeof(b.owner)-1]=0; }
    else return;
    _dirty = true;
  }
  size_t getBytes(const char* k, void* buf, size_t len) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"vel") && len == sizeof(b.vel)) { memcpy(buf, b.vel, len); return len; }
    return 0;
  }
  void putBytes(const char* k, const void* buf, size_t len) {
    _BuddyBlob& b = gBlob();
    if (!strcmp(k,"vel") && len == sizeof(b.vel)) { memcpy(b.vel, buf, len); _dirty = true; }
  }
};
