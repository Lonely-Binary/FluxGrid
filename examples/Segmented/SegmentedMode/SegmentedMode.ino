/*
  Fluxgrid — Segmented (pick a fan speed from a Segmented widget)

  The Segmented widget is a small set of mutually-exclusive options. Selecting one
  writes its INDEX (0, 1, 2, …) to the datastream — the first option is 0, the
  second 1, and so on. It's perfect for modes: Off / Low / High, Auto / Manual,
  Heat / Cool / Fan.

  This sketch maps a three-way fan control (Off / Low / High) onto two relay
  pins — a common two-speed fan wiring.

  Dashboard setup:
    Drop a Segmented widget (Control group); rename its handle to "fan" and set
    its Options to "Off,Low,High" in the Properties panel.

  Wiring:
    GPIO 26 → LOW-speed winding relay   ·   GPIO 27 → HIGH-speed winding relay
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define FAN_HANDLE "fan"   // must match your Segmented widget's datastream handle

const int FAN_LOW  = 26;
const int FAN_HIGH = 27;

void setup() {
  pinMode(FAN_LOW, OUTPUT);
  pinMode(FAN_HIGH, OUTPUT);

  // Segmented widget → the selected option index. 0 = Off, 1 = Low, 2 = High.
  Fluxgrid.onReceive(FAN_HANDLE, [](FluxValue v) {
    int mode = v.asInt();
    digitalWrite(FAN_LOW,  mode == 1 ? HIGH : LOW);
    digitalWrite(FAN_HIGH, mode == 2 ? HIGH : LOW);
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
