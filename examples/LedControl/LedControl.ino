/*
  LedControl — Fluxgrid LED control example

  Demonstrates driving a regular LED and a WS2812 from the same sketch.
  No external LED library needed — WS2812 is driven directly via the ESP32 RMT
  peripheral built into the Fluxgrid library.

  Wiring:
    GPIO 26 → regular LED (+ 330 Ω resistor → GND)
    GPIO 48 → WS2812 data-in (built-in LED on many ESP32-S3 boards)

  Dashboard:
    Create two virtual pins:
      V1 → controls the regular LED   (value "1" = on, "0" = off)
      V2 → controls the WS2812 colour (value "red" | "green" | "blue" | "off")
*/

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"
#include <Fluxgrid.h>

#define PIN_LED    26
#define PIN_WS2812 48

void setup() {
  // Register LEDs before Fluxgrid.begin()
  Fluxgrid.addLed(PIN_LED);                        // regular LED
  Fluxgrid.addLed(PIN_WS2812, LED_WS2812, 0, 80, 255); // WS2812, default blue-ish

  // React to V1: toggle the regular LED
  Fluxgrid.onReceive("V1", [](FluxValue v) {
    if (v.asBool()) Fluxgrid.ledOn(PIN_LED);
    else            Fluxgrid.ledOff(PIN_LED);
  });

  // React to V2: change the WS2812 colour by name
  Fluxgrid.onReceive("V2", [](FluxValue v) {
    String colour = v.asString();
    colour.toLowerCase();
    if      (colour == "red")   Fluxgrid.ledOn(PIN_WS2812, 200,   0,   0);
    else if (colour == "green") Fluxgrid.ledOn(PIN_WS2812,   0, 200,   0);
    else if (colour == "blue")  Fluxgrid.ledOn(PIN_WS2812,   0,   0, 200);
    else                        Fluxgrid.ledOff(PIN_WS2812);
  });

  Fluxgrid.begin();

  // Quick power-on test: flash both LEDs once
  Fluxgrid.ledOn(PIN_LED);
  Fluxgrid.ledOn(PIN_WS2812, 255, 255, 255);
  delay(300);
  Fluxgrid.ledOff(PIN_LED);
  Fluxgrid.ledOff(PIN_WS2812);
}

void loop() {
  Fluxgrid.run();
}
