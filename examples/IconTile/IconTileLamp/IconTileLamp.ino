/*
  Fluxgrid — Icon Tile (one-tap toggle from an Icon Tile widget)

  The Icon Tile widget is a compact, icon-fronted on/off toggle: tapping it
  writes 1 (on) or 0 (off) to its Boolean datastream and the tile lights up in its
  colour. It's the friendliest control for a single appliance — a lamp, a fan, a
  heater — where you want a big, obvious glyph rather than a labelled switch.

  This sketch toggles a WS2812 "lamp" so the tile and the light track each other.

  Dashboard setup:
    Drop an Icon Tile widget (Control group). It creates a Boolean datastream —
    rename its handle to "lamp". Pick a glyph for the tile in the Properties
    panel (e.g. "lightbulb").

  Wiring:
    GPIO 48 → WS2812 data-in (built-in LED on many ESP32-S3 boards)
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define LAMP_HANDLE "lamp"   // must match your Icon Tile widget's datastream handle
#define PIN_WS2812 48

void setup() {
  // Register the WS2812 (warm-white default) before begin().
  Fluxgrid.addLed(PIN_WS2812, LED_WS2812, 255, 180, 80);

  // Icon Tile widget → toggle the lamp. on → last colour, off → dark.
  Fluxgrid.onReceive(LAMP_HANDLE, [](FluxValue v) {
    if (v.asBool()) Fluxgrid.ledOn(PIN_WS2812);
    else            Fluxgrid.ledOff(PIN_WS2812);
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
