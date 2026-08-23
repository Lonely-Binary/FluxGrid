/*
  Fluxgrid — Slider (dim a WS2812 from a Slider widget)

  The Slider widget sends a number between its datastream's min and max as you
  drag it. Here we map that to LED brightness and drive a single WS2812 (the
  on-board RGB LED on many ESP32-S3 boards) through the library's built-in RMT
  driver — no external LED library needed.

  Dashboard setup:
    Drop a Slider widget on the canvas (Control group). It creates a Number
    datastream — rename its handle to "brightness", and set its range to
    min 0 / max 255 so the slider spans the full brightness scale.

  Wiring:
    GPIO 48 → WS2812 data-in (built-in LED on many ESP32-S3 boards)

  Fluxgrid.onReceive() hands the new value to the lambda once per change; we
  clamp it to 0–255 and write it as a white level (equal R/G/B).
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define BRIGHTNESS_HANDLE "brightness" // must match your Slider widget's datastream handle
#define PIN_WS2812 48

void setup() {
  // Register the WS2812 before begin().
  Fluxgrid.addLed(PIN_WS2812, LED_WS2812);

  // Slider (0–255) → white brightness on the WS2812.
  Fluxgrid.onReceive(BRIGHTNESS_HANDLE, [](FluxValue v) {
    int b = v.asInt();
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    Fluxgrid.ledOn(PIN_WS2812, b, b, b);   // equal channels = white at level b
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
