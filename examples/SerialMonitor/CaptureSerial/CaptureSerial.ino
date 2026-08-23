/*
  Fluxgrid — Serial Monitor on the dashboard · Method A: capture Serial

  See your device's serial output in the BROWSER, not just over USB — with
  ZERO changes to how you already log. Drop a Terminal widget on the canvas
  (Display group) and bind it to the "log" datastream; every Serial line below
  shows up there live, and still prints to the USB Serial Monitor as usual.

  How it works
  ────────────
  #define FLUXGRID_CAPTURE_SERIAL 1 (BEFORE the include) aliases `Serial` to a
  tee. Each completed line (on '\n') is sent to BOTH the USB monitor and the
  "log" datastream — your existing Serial.print / println / printf are unchanged.

  This is the "no new API" route. Prefer to tee only specific lines and leave
  Serial completely untouched? See the sibling ExplicitPrint example (Method B).

  Things to know
  ──────────────
   - The macro is a blunt substitution of the `Serial` token, so keep
     #include <Fluxgrid.h> LAST (after any other libraries) — headers included
     after it that use Serial in inline code would get captured too.
   - Lines reach the cloud only once connected; early boot lines are USB-only
     and are not back-filled.
   - The cloud rate-limits one stream to a few lines/sec (USB shows everything).
   - A line that is purely a number (e.g. "42") is treated as a numeric value,
     not a log line. Mixed text is fine.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard

#define FLUXGRID_CAPTURE_SERIAL 1         // tee Serial → USB + dashboard "log"
#include <Fluxgrid.h>                      // ← keep LAST: this aliases the Serial token

void setup() {
  Serial.begin(115200);                   // routed through the tee; real UART still works
  // Serial.setLogHandle("dbg");          // optional: publish to a handle other than "log"
  Fluxgrid.begin();                       // WiFi + cloud, all config built in

  Serial.println("[boot] device starting up");  // USB-only until the cloud link is up
}

unsigned long lastLog = 0;

void loop() {
  Fluxgrid.run();                         // keep the connection alive

  if (millis() - lastLog > 2000) {        // one status line every 2 seconds
    lastLog = millis();

    // Plain Serial — tee'd to USB + dashboard because the capture macro is on.
    Serial.printf("uptime %lus, free heap %u\n",
                  millis() / 1000, ESP.getFreeHeap());

    // The Terminal colours a line by its first word — try the severity prefixes
    // INFO / OK (green·blue), WARN (amber), ERR / ERROR (red):
    if (ESP.getFreeHeap() < 20000) Serial.println("WARN free heap is getting low");
  }
}
