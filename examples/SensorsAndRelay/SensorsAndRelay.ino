/*
  Fluxgrid — Sensors + Relay (a slightly fuller example)
  - Publishes temperature, humidity, and a pump state
  - Controls a relay from a Switch and pulses a pump from a Button
  - Uses an onConnected hook (TLS is on by default)

  Drop the widgets on the canvas and use the handles they create (shown on each
  widget). Here we assume: "temp", "humidity", "relay" (Switch), "pump" (Button)
  and "pumpled" (an LED echoing the relay).
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

const int RELAY_PIN = 26;
const int PUMP_PIN  = 27;

void onLink() {
  // Runs each time we (re)connect — e.g. publish a boot/banner line.
  Fluxgrid.write("temp", 0.0f);
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  // Switch → relay (echo the state to an LED datastream)
  Fluxgrid.onReceive("relay", [](FluxValue v) {
    digitalWrite(RELAY_PIN, v.asBool() ? HIGH : LOW);
    Fluxgrid.write("pumpled", v.asBool());
  });

  // Momentary Button → pulse the pump
  Fluxgrid.onReceive("pump", [](FluxValue v) {
    if (v.asBool()) {
      digitalWrite(PUMP_PIN, HIGH);
      delay(400);
      digitalWrite(PUMP_PIN, LOW);
    }
  });

  Fluxgrid.onConnected(onLink);
  Fluxgrid.begin();                  // WiFi + cloud — TLS built in
}

unsigned long last = 0;

void loop() {
  Fluxgrid.run();

  if (millis() - last > 2000) {
    last = millis();
    float tempC = analogRead(4) * (50.0 / 4095.0);
    float hum   = 40.0 + analogRead(5) * (60.0 / 4095.0);
    Fluxgrid.write("temp", tempC);
    Fluxgrid.write("humidity", hum);
  }
}
