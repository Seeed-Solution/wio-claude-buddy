#if !defined(MOCK_DATA) && defined(BUDDY_BLE)
// Nordic UART Service bridge on the Wio Terminal's RTL8720DN, via Seeed's
// rpcBLE (BLE runs on the wireless co-processor, driven over RPC). The API
// mirrors the ESP32 BLE API the upstream used.
//
// IMPORTANT (see README "Hardware status"): rpcBLE 1.0.0 exposes no
// BLESecurity / LE Secure Connections bonding API. The upstream host asks
// for encrypted-only NUS characteristics + bonding; we cannot satisfy that
// here, so the link is unencrypted and bleSecure() reports false. A host
// that strictly requires encryption may refuse to exchange data — this is
// the known hardware limitation. Everything else (advertise, connect,
// NUS RX/TX, JSON, approvals) works over the open link.
#include "ble_bridge.h"
#include <Arduino.h>
#include <string>
#include "rpcBLEDevice.h"
#include <BLEServer.h>
#include <BLE2902.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static const size_t RX_CAP = 2048;
static uint8_t rxBuf[RX_CAP];
static volatile size_t rxHead = 0, rxTail = 0;
static BLECharacteristic* txChar = nullptr;
static volatile bool connected = false;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t nx = (rxHead + 1) % RX_CAP;
    if (nx == rxTail) return;      // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = nx;
  }
}

static volatile bool needAdv = false;

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (!v.empty()) rxPush((const uint8_t*)v.data(), v.size());
  }
};

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected = true; Serial.println("[ble] connected"); }
  void onDisconnect(BLEServer*) override {
    // Just mark disconnected. We deliberately do NOT restart advertising:
    // rpcBLE's startAdvertising() after a disconnect wedges the RTL8720 and
    // hard-crashes the board (dark screen, needs reflash). Staying alive but
    // not re-advertising is the stable trade-off — reconnect via a reboot.
    connected = false;
    Serial.println("[ble] disconnected");
  }
};

void bleLoop() {
  (void)needAdv;   // advertising restart intentionally disabled (see onDisconnect)
}

void bleInit(const char* name) {
  BLEDevice::init(std::string(name));
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new SrvCb());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);
  txChar = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic* rxChar = svc->createCharacteristic(
    NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCb());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s' (NUS, unencrypted)\n", name);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return false; }   // rpcBLE has no LE SC bonding — see header note
uint32_t blePasskey() { return 0; }
void bleClearBonds() {}                  // no bond store to clear

size_t bleAvailable() { return (rxHead + RX_CAP - rxTail) % RX_CAP; }
int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  const size_t chunk = 180;          // safe ATT notify payload
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(6);                         // let the RPC/BLE stack flush
  }
  return sent;
}
#endif // !MOCK_DATA && BUDDY_BLE
