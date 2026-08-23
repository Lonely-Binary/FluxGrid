/*
  Fluxgrid — ButtonLoop (poll a Button widget from loop())

  This sketch reads a Button widget by POLLING it inside loop() with
  Fluxgrid.read() — no callback. Use this style when you want the button's
  current state to drive something continuously (e.g. hold-to-run a motor),
  or when it's simpler to check the value alongside your other loop logic.

  Drop a Button widget on the canvas; it creates a datastream with a handle
  like "button". Fluxgrid.read("button") returns the latest value the cloud
  has sent — false until the first write arrives. Wrap it in a FluxValue and
  use .asBool() / .asInt() / .asFloat() / .asString() to read it.

  Edge detection: because loop() runs thousands of times per second, we keep
  the previous state in `lastState` and only act when it CHANGES. That turns a
  continuous poll into one "pressed" / "released" event per transition — handy
  for counting presses or logging without a callback.

  Prefer a callback that fires once per incoming write? See the EventButton /
  ButtonOnReceive examples, which use Fluxgrid.onReceive() instead.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define BUTTON_HANDLE "button" // must match your Button widget's datastream handle

bool lastState = false;        // previous button state, for edge detection

void setup() {
  Serial.begin(115200);
  Fluxgrid.begin();            // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();              // keep the connection alive

  // Poll the latest value every loop. false until the first cloud write.
  bool pressed = Fluxgrid.read(BUTTON_HANDLE).asBool();

  // Act only on a CHANGE — one message per press/release, not every loop.
  if (pressed != lastState) {
    lastState = pressed;
    Serial.println(pressed ? "Button is pressed" : "Button is released");
  }
}
