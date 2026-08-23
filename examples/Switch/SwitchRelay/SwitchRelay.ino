/*
  Fluxgrid — Switch (toggle a relay from a Switch widget)

  The Switch widget is an on/off toggle: tapping it writes 1 (on) or 0 (off) to
  its datastream and the widget reflects the pin's reported value. Use it for
  anything latching — a relay, a pump, a mains socket — where the state should
  stay put until you flip it again. (For a momentary push that's only "on" while
  held, use the Button widget instead.)

  Dashboard setup:
    Drop a Switch widget on the canvas (Control group). It creates a Boolean
    datastream — rename its handle to "relay" so it matches the code below.

  Wiring:
    GPIO 26 → relay module IN (or an LED + 330 Ω resistor → GND to test)

  This sketch reacts with Fluxgrid.onReceive(): the cloud calls the lambda once
  per toggle with the new state — true (on) drives the pin HIGH, false (off)
  drives it LOW. No polling or edge detection to write yourself.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define RELAY_HANDLE "relay"   // must match your Switch widget's datastream handle

const int RELAY_PIN = 26;

void setup() {
  pinMode(RELAY_PIN, OUTPUT);

  // Switch widget → drive the relay. Fires once per toggle with the new state.
  Fluxgrid.onReceive(RELAY_HANDLE, [](FluxValue v) {
    digitalWrite(RELAY_PIN, v.asBool() ? HIGH : LOW);
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
