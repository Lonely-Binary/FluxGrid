/*
  Fluxgrid — Serial Monitor on the dashboard

  See your device's serial output in the browser, not just over USB. Drop a
  Terminal widget on the canvas (Display group) and bind it to the "log"
  datastream; every line below shows up there live.

  Two ways to send lines — this sketch shows BOTH:

  A) Zero code change. With FLUXGRID_CAPTURE_SERIAL set to 1 (below, before the
     include), your normal Serial.print / Serial.println / Serial.printf are
     tee'd to BOTH the USB Serial Monitor and the "log" stream. Nothing else in
     your sketch changes.

  B) Explicit. Fluxgrid.println(...) / .printf(...) does the same tee on demand,
     without redefining Serial. Use this if you'd rather not alias Serial (see
     the caveats in the README).

  Both share one log handle — change it with Fluxgrid.setLogHandle("dbg") (or
  Serial.setLogHandle("dbg") when the macro is on) before you print.

  Notes:
   - Lines only reach the cloud once connected; early boot lines are USB-only.
   - The cloud rate-limits a single stream to a few lines/sec — very chatty
     logging will be throttled there (the USB monitor still shows everything).
   - A line that is purely a number (e.g. "42") is treated as a numeric value,
     not a log line. Mixed text is fine.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard

#define FLUXGRID_CAPTURE_SERIAL 1         // tee Serial → USB + dashboard "log"
#include <Fluxgrid.h>

void setup() {
  Serial.begin(115200);                   // routed to the tee; real UART still works
  Fluxgrid.begin();                       // WiFi + cloud, all config built in

  // Captured to the dashboard (route A — plain Serial, no code change):
  Serial.println("[boot] device starting up");
}

unsigned long lastLog = 0;

void loop() {
  Fluxgrid.run();                         // keep the connection alive

  if (millis() - lastLog > 2000) {        // one status line every 2 seconds
    lastLog = millis();

    // Route A: ordinary Serial — tee'd because FLUXGRID_CAPTURE_SERIAL is on.
    Serial.printf("uptime %lus, free heap %u\n",
                  millis() / 1000, ESP.getFreeHeap());

    // Route B: explicit, works with or without the capture macro.
    Fluxgrid.printf("rssi %d dBm\n", WiFi.RSSI());
  }
}
