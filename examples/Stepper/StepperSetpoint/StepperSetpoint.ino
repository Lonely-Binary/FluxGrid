/*
  Fluxgrid — Stepper (a thermostat setpoint from a Stepper widget)

  The Stepper widget is a −/+ counter: each tap writes the new absolute value
  (not a delta) to its datastream, clamped to the datastream's min/max and moving
  by the widget's configured step. It's ideal for a setpoint you nudge up and
  down — a target temperature, a timer, a volume.

  This sketch keeps the latest setpoint and acts like a simple thermostat: it
  reads a temperature every few seconds, publishes it to a "temp" datastream, and
  switches a heater relay on below the setpoint and off above it.

  Dashboard setup:
    Drop a Stepper widget (Control group); rename its handle to "setpoint" and
    set its range (e.g. min 5 / max 30) and step (e.g. 1). Optionally add a
    Gauge or Value widget bound to "temp" to watch the reading.

  Wiring:
    GPIO 26 → heater relay IN   ·   analog sensor on GPIO 4 (swap for your own)
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define SETPOINT_HANDLE "setpoint"   // must match your Stepper widget's datastream handle
#define TEMP_HANDLE     "temp"       // datastream the measured temperature is published on

const int RELAY_PIN = 26;
int setpoint = 22;               // °C target, updated by the Stepper widget

void setup() {
  pinMode(RELAY_PIN, OUTPUT);

  // Stepper widget → store the new setpoint (the absolute value, already clamped).
  Fluxgrid.onReceive(SETPOINT_HANDLE, [](FluxValue v) {
    setpoint = v.asInt();
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

unsigned long last = 0;

void loop() {
  Fluxgrid.run();                // keep the connection alive

  if (millis() - last > 2000) {
    last = millis();
    float tempC = analogRead(4) * (50.0 / 4095.0);   // replace with your sensor
    Fluxgrid.write(TEMP_HANDLE, tempC);
    digitalWrite(RELAY_PIN, tempC < setpoint ? HIGH : LOW);  // heat below target
  }
}
