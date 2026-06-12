/*
  Fluxgrid — EventButton (onReceive demo)

  Use onReceive() when you want a callback fired once per incoming write —
  ideal for momentary buttons, log entries, or anything edge-triggered.

  Drop a Button widget on the canvas; it creates a datastream with a handle
  like "button". Each press fires the lambda below exactly once; read() would
  also work but would need polling to detect the edge. The callback receives a
  FluxValue — use .asBool() / .asInt() / .asFloat() / .asString() to read it.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define OUT_PIN    2            // any output GPIO (flashes on each press)

unsigned long pressCount = 0;

void setup() {
  Serial.begin(115200);
  pinMode(OUT_PIN, OUTPUT);

  // Register event callback before begin()
  Fluxgrid.onReceive("button", [](FluxValue v) {
    if (v.asBool()) {                // button press (value == 1)
      pressCount++;
      Serial.print("Button pressed! Total: ");
      Serial.println(pressCount);
      // Flash the output on each press
      digitalWrite(OUT_PIN, HIGH);
      delay(100);
      digitalWrite(OUT_PIN, LOW);
      // Report count back to the dashboard
      Fluxgrid.write("presses", (long)pressCount);
    }
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();
}
