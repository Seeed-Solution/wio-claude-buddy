#pragma once
// Template credentials for the Indicator spike builds. Copy to
// indicator_secrets.h (gitignored) and fill in. Builds without a secrets
// file fall back to these placeholders so CI compiles — the device just
// won't join a network.
//
// The Indicator is its OWN SenseCraft device (do not reuse the Wio's EUI):
//   sensecraft-cli device devkit create --sku blank_device --name claude-buddy-indicator
//   sensecraft-cli device devkit key --eui <NEW_EUI>
// then add the new EUI/key to host/sensecraft_config.json's "devices" list —
// one bridge process feeds both devices.
//
// WiFi credentials here are TEMPORARY (spike/M3 only): the Phase-2
// provisioning UI (scan + on-screen keyboard + NVS) replaces them.

#define WIFI_SSID     "your-ssid"
#define WIFI_PASS     "your-password"

// OpenStream MQTT broker. Production international: sensecap-openstream.seeed.cc
#define MQTT_HOST     "sensecap-openstream.seeed.cc"
#define MQTT_PORT     1883

#define SC_ORG_ID     "0000000000000000"
#define SC_ACCESS_KEY "your-org-access-key"
#define SC_DEVICE_EUI "0000000000000000"
