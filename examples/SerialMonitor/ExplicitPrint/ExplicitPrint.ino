/*
  Fluxgrid — Serial Monitor on the dashboard · Method B: explicit print

  See your device's serial output in the BROWSER, not just over USB — for the
  lines you choose, with Serial left completely untouched. Drop a Terminal
  widget on the canvas (Display group) and bind it to the "log" datastream.

  How it works
  ────────────
  No macro here. Fluxgrid is a Print, so Fluxgrid.print / println / printf send
  a line to BOTH the USB Serial Monitor and the "log" datastream. Ordinary
  Serial.* still goes to USB only — so you pick, per line, what reaches the cloud.

  This is the explicit route: nothing redefines `Serial`, so it composes cleanly
  with other libraries. Prefer to mirror ALL your existing Serial output with no
  code changes? See the sibling CaptureSerial example (Method A).

  Things to know
  ──────────────
   - Lines reach the cloud only once connected; anything sent earlier is dropped
     on the cloud side (it still prints to USB — Fluxgrid.print always tees there).
   - The cloud rate-limits one stream to a few lines/sec (USB shows everything).
   - A line that is purely a number (e.g. "42") is treated as a numeric value,
     not a log line. Mixed text is fine.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

void setup() {
  Serial.begin(115200);                   // stock Arduino Serial — USB only, untouched
  // Fluxgrid.setLogHandle("dbg");        // optional: publish to a handle other than "log"
  Fluxgrid.begin();                       // WiFi + cloud, all config built in

  Serial.println("[boot] this line is USB-only (stock Serial)");  // → USB monitor only
  Fluxgrid.println("[boot] device starting up");                  // → USB + dashboard "log"
}

unsigned long lastLog = 0;

void loop() {
  Fluxgrid.run();                         // keep the connection alive

  if (millis() - lastLog > 2000) {        // one status line every 2 seconds
    lastLog = millis();

    // Explicit tee — these reach the Terminal widget AND the USB monitor.
    Fluxgrid.printf("uptime %lus, free heap %u\n",
                    millis() / 1000, ESP.getFreeHeap());
    Fluxgrid.printf("rssi %d dBm\n", WiFi.RSSI());

    // The Terminal colours a line by its first word — INFO / OK (green·blue),
    // WARN (amber), ERR / ERROR (red):
    if (ESP.getFreeHeap() < 20000) Fluxgrid.println("WARN free heap is getting low");
  }
}
