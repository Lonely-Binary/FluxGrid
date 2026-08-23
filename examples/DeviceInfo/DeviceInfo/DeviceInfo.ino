#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

// The Device Info widget needs NO extra code. Hardware & network facts are
// reported automatically when the device connects — the library publishes the
// board's chip / flash / PSRAM / MACs / SDK plus the current IP / gateway / DNS /
// RSSI on connect. The Network section (IP / gateway / DNS) refreshes on every
// reconnect. Requires Fluxgrid >= 0.14.0.

void setup() {
  Fluxgrid.begin();   // WiFi + cloud, all config built in — reports device info on connect
}

void loop() {
  Fluxgrid.run();     // keep the link alive; device info is re-published on every reconnect
}