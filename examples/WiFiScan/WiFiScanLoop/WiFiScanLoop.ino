/*
  Fluxgrid — WiFiScanLoop (publish a WiFi scan to a WiFi Scan widget)

  This sketch scans for nearby WiFi networks on a timer and publishes them with
  a single call: Fluxgrid.writeWiFiScan(). The library does the scan, sorts the
  networks strongest-first, builds the JSON, and publishes it to the datastream
  — you just choose how often to run it.

  Dashboard setup
  ───────────────
  1. Create a datastream named "wifi_scan" (kind: json).
  2. Drop a WiFi Scan widget on the canvas and bind it to "wifi_scan".
  3. Flash this sketch with your credentials filled in below.

  writeWiFiScan() BLOCKS while the radio scans (1–4 s), so call it on an
  interval — not every loop(). For a version that keeps the connection
  responsive during the scan, see the WiFiScanAsync example.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

// How often to scan (ms). The scan itself takes 1–4 s, so keep this comfortably
// larger — 10 s is a good default.
static const unsigned long SCAN_INTERVAL = 10000UL;

unsigned long lastScan = 0;

void setup() {
  Serial.begin(115200);
  Fluxgrid.begin();              // server, port and TLS are built in
  Fluxgrid.writeWiFiScan();      // first scan right away so the widget fills in
  lastScan = millis();
}

void loop() {
  Fluxgrid.run();                // keep the connection alive

  if (millis() - lastScan >= SCAN_INTERVAL) {
    lastScan = millis();
    int n = Fluxgrid.writeWiFiScan();   // publishes to "wifi_scan"; returns count
    Serial.printf("Published %d networks\n", n);
  }
}
