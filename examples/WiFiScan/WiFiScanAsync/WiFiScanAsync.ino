/*
  Fluxgrid — WiFiScanAsync (non-blocking WiFi scan to a WiFi Scan widget)

  Same result as WiFiScanLoop, but the scan runs in the background so the cloud
  connection stays responsive the whole time. A blocking scan freezes loop()
  for 1–4 s, during which Fluxgrid.run() can't service MQTT keepalive; the async
  pattern avoids that, which matters if your sketch also pushes other telemetry
  or reacts to control widgets.

  How it works
  ────────────
  • Fluxgrid.startWiFiScan()  kicks the radio and returns immediately.
  • Fluxgrid.pollWiFiScan()   called every loop(), returns:
        -1  still scanning  (keep polling)
        -2  no scan running (start one)
       >=0  done — it just published the results; value is the network count.

  Dashboard setup is identical to WiFiScanLoop: a "wifi_scan" json datastream
  bound to a WiFi Scan widget.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

// How long to wait between finishing one scan and starting the next (ms).
static const unsigned long SCAN_GAP = 10000UL;

bool          scanning = false;  // is a scan currently in flight?
unsigned long lastDone = 0;      // millis() when the last scan finished

void setup() {
  Serial.begin(115200);
  Fluxgrid.begin();              // server, port and TLS are built in
  Fluxgrid.startWiFiScan();      // kick the first scan
  scanning = true;
}

void loop() {
  Fluxgrid.run();                // stays responsive — the scan runs in the background

  if (scanning) {
    int n = Fluxgrid.pollWiFiScan();   // publishes to "wifi_scan" when ready
    if (n >= 0) {                       // finished (n = networks published)
      Serial.printf("Published %d networks\n", n);
      scanning = false;
      lastDone = millis();
    }
  } else if (millis() - lastDone >= SCAN_GAP) {
    Fluxgrid.startWiFiScan();           // time for the next one
    scanning = true;
  }
}
