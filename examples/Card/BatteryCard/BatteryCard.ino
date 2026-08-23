/*
  Fluxgrid — Card (a battery percentage with a small "%" affix)

  A companion to TemperatureCard that shows the *other* unit style: a "%" reads
  best as a small superscript-ish affix next to the number, which is the Card's
  default. So this example leaves Unit size on "Small" — the contrast with the
  Temperature card's "Normal" °C is the whole point.

  This sketch converts a battery voltage (read through a divider) to a 0–100 %
  charge estimate and publishes it.

  Dashboard setup:
    1. Drop a Card widget and rename its datastream handle to "battery".
    2. In the Properties panel:
         • Icon      → "battery"
         • Postfix   → "%"
         • Unit size → Small      (the default — keeps "%" as a compact affix)
         • Decimals  → 0
         • Conditional icon color → when value < 20, On color = red
    3. (Optional) "Save as template" → "Battery".

  Wiring:
    Battery + → R1 → ADC_PIN → R2 → GND  (a divider that keeps the pin under the
    ADC's max). Set DIVIDER to (R1 + R2) / R2 and pick your full/empty voltages.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define BATTERY_HANDLE "battery"   // must match your Card widget's datastream handle
#define ADC_PIN     4
static const unsigned long SEND_INTERVAL = 5000UL;   // charge changes slowly

// Divider ratio and the ADC reference (ESP32: 3.3 V full-scale ≈ 4095 counts).
static const float DIVIDER  = 2.0f;     // (R1 + R2) / R2 — adjust to your resistors
static const float ADC_REF  = 3.3f;
static const float ADC_MAX  = 4095.0f;

// A single-cell Li-ion maps ~3.3 V (empty) → ~4.2 V (full). Tune for your pack.
static const float V_EMPTY  = 3.3f;
static const float V_FULL   = 4.2f;

void setup() {
  Fluxgrid.begin();             // server, port and TLS are built in
}

unsigned long lastSend = 0;

void loop() {
  Fluxgrid.run();               // keep the connection alive

  if (millis() - lastSend < SEND_INTERVAL) return;
  lastSend = millis();

  float vBat = analogRead(ADC_PIN) * (ADC_REF / ADC_MAX) * DIVIDER;
  float pct  = (vBat - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);   // clamp to 0–100

  Fluxgrid.write(BATTERY_HANDLE, (int)lroundf(pct));  // the Card shows e.g. "84 %"
}
