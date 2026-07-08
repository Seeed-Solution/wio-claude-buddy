#pragma once
// Template credentials for the NET (WiFi + SenseCraft MQTT) build.
//
// Copy to secrets.h (gitignored) and fill in. Builds without a secrets.h
// fall back to these placeholders so CI compiles — the device just won't
// join a network.
//
// SC_ORG_ID / SC_ACCESS_KEY come from the SenseCraft portal (API key page);
// SC_DEVICE_EUI is the blank_device devkit created with:
//   sensecraft-cli device devkit create --sku blank_device --name claude-buddy

#define WIFI_SSID     "your-ssid"
#define WIFI_PASS     "your-password"

// OpenStream MQTT broker. Production international: sensecap-openstream.seeed.cc
// (the Develop test env has no public OpenStream broker — use production, or
// the internal develop host if you have it).
#define MQTT_HOST     "sensecap-openstream.seeed.cc"
#define MQTT_PORT     1883

#define SC_ORG_ID     "0000000000000000"
#define SC_ACCESS_KEY "your-org-access-key"
#define SC_DEVICE_EUI "0000000000000000"
