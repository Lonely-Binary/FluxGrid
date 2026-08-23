/*
  Fluxgrid — Switch Group (control a room of relays from one card)

  The Switch Group widget is an entities card: it binds several Boolean
  datastreams and shows one labelled toggle per row, plus a master switch in the
  header that flips them all at once. Each row writes 1/0 to its OWN datastream,
  so on the device you simply handle each handle separately — exactly like several
  individual Switch widgets, grouped into one tidy card.

  This sketch drives three light relays for a "Living Room" group.

  Dashboard setup:
    Drop a Switch Group widget (Control group). Add three rows by binding three
    Boolean datastreams — name their handles "light1", "light2", "light3". In the
    Properties panel you can rename each row and give it its own icon.

  Wiring:
    GPIO 25 → light 1 relay   ·   GPIO 26 → light 2 relay   ·   GPIO 27 → light 3 relay
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

// One handle per row — must match the Boolean datastreams bound to your widget.
#define LIGHT1_HANDLE "light1"
#define LIGHT2_HANDLE "light2"
#define LIGHT3_HANDLE "light3"

const int LIGHT1 = 25;
const int LIGHT2 = 26;
const int LIGHT3 = 27;

void setup() {
  pinMode(LIGHT1, OUTPUT);
  pinMode(LIGHT2, OUTPUT);
  pinMode(LIGHT3, OUTPUT);

  // One callback per row — each row writes to its own datastream handle. The
  // group's master toggle simply flips every row, so these handlers cover it too.
  Fluxgrid.onReceive(LIGHT1_HANDLE, [](FluxValue v) { digitalWrite(LIGHT1, v.asBool() ? HIGH : LOW); });
  Fluxgrid.onReceive(LIGHT2_HANDLE, [](FluxValue v) { digitalWrite(LIGHT2, v.asBool() ? HIGH : LOW); });
  Fluxgrid.onReceive(LIGHT3_HANDLE, [](FluxValue v) { digitalWrite(LIGHT3, v.asBool() ? HIGH : LOW); });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
