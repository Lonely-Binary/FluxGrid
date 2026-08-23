/*
  Fluxgrid — ButtonOnReceive (onReceive callback for a Button widget)

  This sketch reacts to a Button widget with Fluxgrid.onReceive(): the cloud
  calls your lambda once per incoming write, so there's no polling and no edge
  detection to write yourself. Ideal for momentary buttons and anything
  edge-triggered.

  Drop a Button widget on the canvas; it creates a datastream with a handle
  like "button". Wire that handle into onReceive() below. The callback
  receives a FluxValue — use .asBool() / .asInt() / .asFloat() / .asString()
  to read it. Here .asBool() is true on press, false on release.

  Register the callback BEFORE Fluxgrid.begin(). Want to poll the value from
  loop() instead of using a callback? See the ButtonLoop example.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define BUTTON_HANDLE "button" // must match your Button widget's datastream handle

void setup() {
  Serial.begin(115200);

  // Register the event callback before begin(). Fires once per incoming write.
  Fluxgrid.onReceive(BUTTON_HANDLE, [](FluxValue v) {
    Serial.println(v.asBool() ? "Button is pressed" : "Button is released");
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
