/*
  Fluxgrid — RGB (drive a WS2812 from an RGB picker widget)

  The RGB widget edits one colour but sends it as THREE separate channel values —
  red, green and blue, each a Number 0–255 on its own datastream. So as you drag,
  each channel arrives as its own message. We keep the latest R/G/B here and push
  the combined colour to a WS2812 whenever any channel changes.

  Dashboard setup:
    Drop an RGB widget (Control group). It binds three Number datastreams — name
    their handles "red", "green" and "blue" and wire them into the RGB widget's
    R / G / B channel selectors.

  Wiring:
    GPIO 48 → WS2812 data-in (built-in LED on many ESP32-S3 boards)

  Note — one message vs three: the RGB widget always writes its three channels
  separately. If you'd rather receive a colour in a SINGLE message, use the Color
  widget instead: it packs the colour into one 24-bit integer (0xRRGGBB) on one
  datastream, which you unpack on the device with
    int rgb = Fluxgrid.read("color").asInt();
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

// One handle per RGB channel — must match the R / G / B datastreams on your widget.
#define RED_HANDLE   "red"
#define GREEN_HANDLE "green"
#define BLUE_HANDLE  "blue"
#define PIN_WS2812 48

int R = 0, G = 0, B = 0;         // latest channel values

void applyColor() {
  Fluxgrid.ledOn(PIN_WS2812, R, G, B);
}

void setup() {
  // Register the WS2812 before begin().
  Fluxgrid.addLed(PIN_WS2812, LED_WS2812);

  // Each channel arrives on its own datastream; remember it and refresh the LED.
  Fluxgrid.onReceive(RED_HANDLE,   [](FluxValue v) { R = v.asInt(); applyColor(); });
  Fluxgrid.onReceive(GREEN_HANDLE, [](FluxValue v) { G = v.asInt(); applyColor(); });
  Fluxgrid.onReceive(BLUE_HANDLE,  [](FluxValue v) { B = v.asInt(); applyColor(); });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive
}
