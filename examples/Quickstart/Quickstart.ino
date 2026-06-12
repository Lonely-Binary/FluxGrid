/*
  Fluxgrid — Quickstart
  Push one sensor value, read one control widget. That's the whole device.

  1. In your Fluxgrid dashboard: add a device → "Pair device" → copy the token.
  2. Drop a Gauge and a Switch on the canvas. Each one creates its own
     datastream — note the handle shown on the widget (e.g. "temp", "relay").
  3. Fill the 3 #defines below, use those handles, flash, done.

  Server, port and TLS are built into the library — Fluxgrid.begin() takes
  no arguments. The credentials must be #defined BEFORE #include <Fluxgrid.h>.
  FG_TOKEN is one string per device; the library splits it into the routing
  token + MQTT user/pass for you.

  Fluxgrid.write("temp", value)  — push a reading to the dashboard
  Fluxgrid.read("relay")         — receive the latest value from a control widget
                                   (.asBool() / .asInt() / .asFloat() / .asString())
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>

#define OUT_PIN    2            // any output GPIO (drives a LED/relay from "relay")

void setup() {
  pinMode(OUT_PIN, OUTPUT);
  Fluxgrid.begin();                       // WiFi + cloud, all config built in
}

unsigned long lastSend = 0;

void loop() {
  Fluxgrid.run();                         // keep the connection alive

  // Read: apply the latest switch value to the output (false until first cloud write)
  digitalWrite(OUT_PIN, Fluxgrid.read("relay").asBool() ? HIGH : LOW);

  if (millis() - lastSend > 2000) {       // send a reading every 2 seconds
    lastSend = millis();
    float tempC = analogRead(4) * (50.0 / 4095.0);  // replace with your sensor
    Fluxgrid.write("temp", tempC);
  }
}
