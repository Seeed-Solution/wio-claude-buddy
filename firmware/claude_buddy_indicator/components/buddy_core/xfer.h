#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "b64.h"
#include "transport.h"
#include "wio_platform.h"
#include "stats.h"

// Forward decls (defined in character.h / stats.h). Declared here because
// data.h includes this header and dispatches "cmd" JSON through xferCommand().
void characterClose();
bool characterInit(const char* name);
void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();

#ifdef BUDDY_FS
// ── hardware: BLE folder-push receiver over Seeed FS ─────────────────────────
#include <Seeed_FS.h>
#include "SD/Seeed_SD.h"
#define BUDDYFS SD

static File     _xFile;
static uint32_t _xExpected = 0, _xWritten = 0;
static char     _xCharName[24] = "";
static bool     _xActive = false;
static uint32_t _xTotal = 0, _xTotalWritten = 0;

// Ack goes to both streams — we don't track which one delivered the command,
// and writes to a clientless link just drop. The bridge listens on whichever
// port it opened.
static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok?"true":"false", (unsigned long)n);
  Serial.write(b, len);
  trWrite((const uint8_t*)b, len);
}

static uint32_t _xWipeDir(const char* dir) {
  File d = BUDDYFS.open(dir);
  if (!d || !d.isDirectory()) { BUDDYFS.mkdir(dir); return 0; }
  uint32_t freed = 0;
  File f = d.openNextFile();
  while (f) {
    freed += f.size();
    char p[80];
    snprintf(p, sizeof(p), "%s/%s", dir, f.name());
    f.close();
    BUDDYFS.remove(p);
    f = d.openNextFile();
  }
  d.close();
  return freed;
}

// Only one character lives on the device at a time. Installing a new one
// under a different name would otherwise leave the old one's files eating
// space. Wipe everything under /characters/, return total bytes reclaimed.
static uint32_t _xWipeAllChars() {
  File root = BUDDYFS.open("/characters");
  if (!root || !root.isDirectory()) { BUDDYFS.mkdir("/characters"); return 0; }
  uint32_t freed = 0;
  File sub = root.openNextFile();
  while (sub) {
    if (sub.isDirectory()) {
      char p[64];
      snprintf(p, sizeof(p), "/characters/%s", sub.name());
      sub.close();
      freed += _xWipeDir(p);
      BUDDYFS.rmdir(p);
    } else {
      sub.close();
    }
    sub = root.openNextFile();
  }
  root.close();
  return freed;
}

// Called from data.h when incoming JSON has a "cmd" key. Returns true if
// it was a transfer command (caller should skip state-update parsing).
inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    extern bool buddyMode, gifAvailable;
    extern void buddySetSpeciesIdx(uint8_t);
    uint8_t idx = doc["idx"] | 0xFF;
    speciesIdxSave(idx);
    buddyMode = !(gifAvailable && idx == 0xFF);
    if (buddyMode) buddySetSpeciesIdx(idx);
    _xAck("species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    trClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Dump everything the info screens show. Manual printf rather than
    // ArduinoJson serialize — less heap churn, and the shape is fixed.
    // The Wio has no battery gauge, so the "bat" object is omitted entirely.
    char b[300];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"sys\":{\"up\":%lu,\"heap\":%lu,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), trSecure() ? "true" : "false",
      (unsigned long)(millis() / 1000), (unsigned long)freeHeapApprox(),
      (unsigned long)(BUDDYFS.totalBytes() - BUDDYFS.usedBytes()),
      (unsigned long)BUDDYFS.totalBytes(),
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  if (strcmp(cmd, "char_begin") == 0) {
    const char* name = doc["name"] | "pet";
    _xTotal = doc["total"] | 0;

    // Fit check: free space after wiping everything under /characters/.
    uint32_t freeB = BUDDYFS.totalBytes() - BUDDYFS.usedBytes();
    uint32_t reclaimable = 0;
    {
      File r = BUDDYFS.open("/characters");
      if (r && r.isDirectory()) {
        File s = r.openNextFile();
        while (s) {
          if (s.isDirectory()) {
            File f = s.openNextFile();
            while (f) { reclaimable += f.size(); f.close(); f = s.openNextFile(); }
          }
          s.close(); s = r.openNextFile();
        }
        r.close();
      }
    }
    uint32_t available = freeB + reclaimable;
    if (_xTotal > 0 && _xTotal + 4096 > available) {
      char b[96];
      int len = snprintf(b, sizeof(b),
        "{\"ack\":\"char_begin\",\"ok\":false,\"n\":%lu,\"error\":\"need %luK, have %luK\"}\n",
        (unsigned long)available, (unsigned long)(_xTotal/1024), (unsigned long)(available/1024)
      );
      Serial.write(b, len);
      trWrite((const uint8_t*)b, len);
      return true;
    }

    strncpy(_xCharName, name, sizeof(_xCharName)-1); _xCharName[sizeof(_xCharName)-1]=0;
    characterClose();
    _xWipeAllChars();
    char dir[48]; snprintf(dir, sizeof(dir), "/characters/%s", _xCharName);
    BUDDYFS.mkdir(dir);
    _xTotalWritten = 0;
    _xActive = true;
    _xAck("char_begin", true);
    return true;
  }

  if (!_xActive) return strcmp(cmd, "permission") != 0;  // permission cmd is not ours

  if (strcmp(cmd, "file") == 0) {
    const char* path = doc["path"];
    _xExpected = doc["size"] | 0;
    _xWritten = 0;
    if (!path) { _xAck("file", false); return true; }
    char full[80]; snprintf(full, sizeof(full), "/characters/%s/%s", _xCharName, path);
    _xFile = BUDDYFS.open(full, "w");
    _xAck("file", (bool)_xFile);
    return true;
  }

  if (strcmp(cmd, "chunk") == 0) {
    const char* b64 = doc["d"];
    if (!b64 || !_xFile) { _xAck("chunk", false); return true; }
    uint8_t buf[300];
    size_t outLen = 0;
    int rc = b64_decode((const uint8_t*)b64, strlen(b64), buf, sizeof(buf), &outLen);
    if (rc != 0) { _xAck("chunk", false); return true; }
    _xFile.write(buf, outLen);
    _xWritten += outLen;
    _xTotalWritten += outLen;
    // Ack every chunk — flash writes can block and the RX buffer is small.
    _xAck("chunk", true, _xWritten);
    return true;
  }

  if (strcmp(cmd, "file_end") == 0) {
    bool ok = _xFile && (_xWritten == _xExpected || _xExpected == 0);
    if (_xFile) _xFile.close();
    _xAck("file_end", ok, _xWritten);
    return true;
  }

  if (strcmp(cmd, "char_end") == 0) {
    _xActive = false;
    bool ok = characterInit(_xCharName);
    extern bool buddyMode, gifAvailable;
    if (ok) { buddyMode = false; gifAvailable = true; speciesIdxSave(0xFF); }
    _xAck("char_end", ok);
    return true;
  }

  return false;
}

inline bool xferActive() { return _xActive; }
inline uint32_t xferProgress() { return _xTotalWritten; }
inline uint32_t xferTotal() { return _xTotal; }

#else  // no GIF/FS build (default hardware): transfers never occur

inline bool xferCommand(JsonDocument&) { return false; }
inline bool xferActive() { return false; }
inline uint32_t xferProgress() { return 0; }
inline uint32_t xferTotal() { return 0; }

#endif // BUDDY_FS
