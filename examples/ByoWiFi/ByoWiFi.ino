/*
  ByoWiFi — "bring your own WiFi".

  By default Fluxgrid manages WiFi for you (see the Quickstart). Call
  Fluxgrid.manageWiFi(false) when you'd rather own the radio yourself — to use
  WiFiManager / Improv for provisioning, to run AP or AP+STA, or to share the
  connection with your own code. The library then only talks to the cloud over
  whatever link you bring up; it never changes WiFi mode, never kicks a
  reconnect, and never blocks.

  Here we connect STA by hand, but the same pattern works with any provisioning
  library: get WiFi up however you like, then begin().
*/
#define FG_TOKEN "DEVICE_TOKEN"     // from the dashboard — no WIFI_SSID/PASS needed
#include <Fluxgrid.h>
#include <WiFi.h>

const char *WIFI_SSID = "your-wifi";
const char *WIFI_PASS = "your-pass";

void setup() {
  Serial.begin(115200);

  // 1) Bring up WiFi yourself (or hand off to WiFiManager / Improv here).
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.printf("\nWiFi up, IP %s\n", WiFi.localIP().toString().c_str());

  // 2) Tell Fluxgrid not to touch the radio, then connect the cloud.
  Fluxgrid.manageWiFi(false);
  Fluxgrid.begin();                 // cloud only — rides on the link above

  // Want the device's own hotspot too (e.g. for a local dashboard)?
  //   Fluxgrid.startAP("Fluxgrid-Device", "fluxgrid123");   // AP+STA
  // Pure offline, no cloud at all?
  //   Fluxgrid.cloud(false); before begin().
}

void loop() {
  Fluxgrid.run();                   // maintains the cloud link; leaves WiFi alone
  Fluxgrid.write("temp", analogRead(4) * (50.0 / 4095.0));
  delay(2000);
}
