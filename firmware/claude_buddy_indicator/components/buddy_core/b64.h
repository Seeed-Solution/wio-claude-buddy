#pragma once
#include <stddef.h>
#include <stdint.h>

// Minimal base64 decoder — drop-in replacement for mbedtls_base64_decode's
// usage in xfer.h. Returns 0 on success, nonzero on error (bad char or
// output buffer too small). Skips ASCII whitespace. *olen = bytes written.
static inline int b64_decode(const unsigned char* in, size_t ilen,
                             unsigned char* out, size_t omax, size_t* olen) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  size_t o = 0;
  int quad[4], qn = 0;
  for (size_t i = 0; i < ilen; i++) {
    unsigned char c = in[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    if (c == '=') break;            // padding — stop accumulating
    int v = val(c);
    if (v < 0) { if (olen) *olen = o; return -1; }
    quad[qn++] = v;
    if (qn == 4) {
      if (o + 3 > omax) { if (olen) *olen = o; return -1; }
      out[o++] = (quad[0] << 2) | (quad[1] >> 4);
      out[o++] = ((quad[1] & 0xF) << 4) | (quad[2] >> 2);
      out[o++] = ((quad[2] & 0x3) << 6) | quad[3];
      qn = 0;
    }
  }
  if (qn == 2) {                    // 1 trailing byte
    if (o + 1 > omax) { if (olen) *olen = o; return -1; }
    out[o++] = (quad[0] << 2) | (quad[1] >> 4);
  } else if (qn == 3) {             // 2 trailing bytes
    if (o + 2 > omax) { if (olen) *olen = o; return -1; }
    out[o++] = (quad[0] << 2) | (quad[1] >> 4);
    out[o++] = ((quad[1] & 0xF) << 4) | (quad[2] >> 2);
  }
  if (olen) *olen = o;
  return 0;
}
