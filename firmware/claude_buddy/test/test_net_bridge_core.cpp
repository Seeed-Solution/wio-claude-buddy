// Native unit test for net_bridge_core.h (no board needed):
//   g++ -std=c++17 firmware/claude_buddy/test/test_net_bridge_core.cpp \
//       -o /tmp/test_nbc && /tmp/test_nbc
//
// Drives the core with fake topics/payloads and a fake clock, then asserts
// the exact JSON lines the Wio firmware and the Indicator firmware must
// both synthesize — this is the wire-format regression test behind the
// shared-file diff-guard.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "../net_bridge_core.h"

static const char* TOPIC_FMT = "/device_sensor_data/999/2CF7F1C0000000AA/1/vs/%d";

static void feed(int measId, const char* payload, uint32_t nowMs) {
  char topic[96];
  snprintf(topic, sizeof(topic), TOPIC_FMT, measId);
  nbcOnPayload(topic, (const uint8_t*)payload, strlen(payload), nowMs);
}

static void feedValue(int measId, double v, uint64_t tsMs, uint32_t nowMs) {
  char body[96];
  snprintf(body, sizeof(body), "{\"value\":%.0f,\"timestamp\":%llu}",
           v, (unsigned long long)tsMs);
  feed(measId, body, nowMs);
}

// Drain one \n-terminated line out of the ring ("" when empty).
static std::string readLine() {
  std::string s;
  while (nbcAvailable()) {
    int b = nbcRead();
    if (b < 0 || b == '\n') return s;
    s += (char)b;
  }
  return s;
}

int main() {
  const uint64_t TS = 1751970000000ULL;   // host wall clock, ms

  // (a) heartbeat group: burst 4097-4101, quiet-flush after 500 ms
  uint32_t now = 1000;
  feedValue(4097, 3, TS, now);        // total
  feedValue(4098, 2, TS, now + 10);   // running
  feedValue(4099, 1, TS, now + 20);   // waiting
  feedValue(4100, 12345, TS, now + 30);
  feedValue(4101, 678, TS, now + 40);
  nbcTick(now + 100);                 // quiet not reached
  assert(nbcAvailable() == 0);
  nbcTick(now + 40 + 500);            // quiet reached
  assert(readLine() ==
         "{\"total\":3,\"running\":2,\"waiting\":1,\"msg\":\"2 active\","
         "\"tokens\":12345,\"tokens_today\":678}");

  // (d-part1) no tz measurement yet -> no time-sync line
  assert(nbcAvailable() == 0);

  // (b) usage group -> v:1 line
  now = 5000;
  feedValue(4102, 42, TS, now);       // session.pct
  feedValue(4103, 3600, TS, now);     // session.reset_s
  feedValue(4104, 17, TS, now);       // week.pct
  feedValue(4105, 90000, TS, now);    // week.reset_s
  feedValue(4106, 55555, TS, now);    // today.tokens
  nbcTick(now + 500);
  assert(readLine() ==
         "{\"v\":1,\"session\":{\"pct\":42,\"reset_s\":3600},"
         "\"week\":{\"pct\":17,\"reset_s\":90000},\"today\":{\"tokens\":55555}}");
  assert(nbcAvailable() == 0);

  // (c) continuous updates: max-flush fires at 2500 ms even with no quiet gap
  now = 10000;
  for (uint32_t t = 0; t <= 2400; t += 100) {
    feedValue(4102, 43, TS, now + t);
    nbcTick(now + t);
    assert(nbcAvailable() == 0);      // neither quiet nor max reached
  }
  feedValue(4102, 44, TS, now + 2500);
  nbcTick(now + 2500);                // dirtySince == 2500 ms ago
  {
    std::string l = readLine();
    assert(l.find("\"pct\":44") != std::string::npos);
  }

  // (d) time sync: biased tz + QUOTED value/timestamp (as the broker sends)
  now = 20000;
  feed(4108, "{\"value\":\"115200\",\"timestamp\":\"1751970005000\"}", now); // utc+8
  feedValue(4097, 3, TS, now);        // heartbeat carries the time-sync line;
  nbcTick(now + 500);                 // lastTs comes from ANY payload -> TS
  assert(readLine().substr(0, 8) == "{\"total\"");
  assert(readLine() == "{\"time\":[1751970000,28800]}");

  // (d-part2) suppressed for 10 min after
  now += 5000;
  feedValue(4097, 3, TS, now);
  nbcTick(now + 500);
  assert(readLine().substr(0, 8) == "{\"total\"");
  assert(nbcAvailable() == 0);        // no second time line

  // (e) idle heartbeat message
  now = 40000;
  feedValue(4098, 0, TS, now);
  nbcTick(now + 500);
  {
    std::string l = readLine();
    assert(l.find("\"msg\":\"idle\"") != std::string::npos);
  }

  // (f) unknown / unused measurement IDs are ignored
  now = 50000;
  feedValue(4107, 88, TS, now);       // fable.pct: not on the NET wire
  feedValue(9999, 1, TS, now);
  feed(4102, "no json here", now);    // value key missing -> ignored
  nbcTick(now + 3000);
  assert(nbcAvailable() == 0);

  // (g) oversized payload dropped, ring survives byte-exact after all this
  now = 60000;
  {
    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    feed(4102, big, now);
    nbcTick(now + 3000);
    assert(nbcAvailable() == 0);
  }

  printf("OK — net_bridge_core wire format verified\n");
  return 0;
}
