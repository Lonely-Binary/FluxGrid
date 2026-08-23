/*
  Fluxgrid — Card (a temperature reading in a configurable value Card)

  The Card widget is a compact value tile: a tinted icon next to a big reading,
  with an optional label and a prefix/postfix around the number. It's the
  friendliest way to surface a single sensor value at a glance.

  The firmware side is deliberately tiny — you just write() a number. Everything
  that makes the Card look good (icon, unit, colour) is configured on the
  dashboard, so most of what's worth knowing is in the "Dashboard setup" below.

  Dashboard setup:
    1. Drop a Card widget (Display group). It creates a number datastream —
       rename its handle to "temp".
    2. In the Properties panel:
         • Icon      → "thermometer"
         • Postfix   → "°C"
         • Unit size → Normal   (a temperature unit reads as part of the number,
                                  unlike % or $ which look better as a small affix)
         • Decimals  → 1
         • Conditional icon color → when value > 30, On color = red
    3. (Optional) Click "Save as template", name it "Temperature", and every
       user can apply that exact look to a new Card in one click.

  Wiring:
    Replace the analogRead() below with your real sensor (DHT, DS18B20, etc.).
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define TEMP_HANDLE "temp"   // must match your Card widget's datastream handle

// How often to publish a reading (ms). Temperature drifts slowly; 2 s is plenty.
static const unsigned long SEND_INTERVAL = 2000UL;

// Raw analog samples jitter, so smooth them with a simple exponential moving
// average — a steadier number makes the Card readable. Smaller alpha = smoother.
static const float EMA_ALPHA = 0.30f;
static float tempAvg = NAN;     // NAN until the first sample seeds it

void setup() {
  Fluxgrid.begin();             // WiFi + cloud; server, port and TLS are built in
}

unsigned long lastSend = 0;

void loop() {
  Fluxgrid.run();               // keep the connection alive

  if (millis() - lastSend < SEND_INTERVAL) return;
  lastSend = millis();

  // Demo reading: scale a 12-bit ADC to a 0–50 °C range. Swap in your sensor.
  float tempC = analogRead(4) * (50.0f / 4095.0f);
  tempAvg = isnan(tempAvg) ? tempC : (1.0f - EMA_ALPHA) * tempAvg + EMA_ALPHA * tempC;

  Fluxgrid.write(TEMP_HANDLE, tempAvg);   // the Card shows it as e.g. "23.4 °C"
}
