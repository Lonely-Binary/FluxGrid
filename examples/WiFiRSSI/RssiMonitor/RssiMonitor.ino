/*
  RssiMonitor.ino — publish the device's own WiFi signal strength to a
  Fluxgrid WiFi RSSI widget.

  Unlike the WiFiScan example (which lists *every* nearby access point), this
  sketch reports the link strength of the connection the ESP32 is *joined to*
  — WiFi.RSSI() with no argument, in dBm. That single number is everything the
  WiFi RSSI widget needs: it derives the quality %, the strong/weak tier, and
  the rolling history graph on its own.

  Dashboard setup
  ───────────────
  1. Create a datastream named "rssi" with kind "number".
  2. Drop a WiFi RSSI widget on the canvas and bind it to "rssi".
  3. Flash this sketch with your credentials filled in below.

  RSSI scale (dBm — closer to zero is stronger)
  ─────────────────────────────────────────────
    ≥ -57  strong     -57…-70  moderate
    -70…-82 weak      < -82    very weak / may drop

  Requirements
  ────────────
  ESP32 Arduino core + PubSubClient + Fluxgrid (all from Library Manager / ZIP).
*/

#define WIFI_SSID  "your-wifi-ssid"
#define WIFI_PASS  "your-wifi-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>

#define RSSI_HANDLE "rssi"   // must match your WiFi RSSI widget's datastream handle

// How often to publish a reading (ms). RSSI changes slowly; 3 s is plenty.
static const unsigned long SEND_INTERVAL = 3000UL;

// Raw RSSI jumps ±5 dBm sample-to-sample, so smooth it with a simple
// exponential moving average — a steadier number makes the gauge readable.
// avg = (1 - A)*avg + A*sample;  smaller A = smoother but slower to react.
static const float EMA_ALPHA = 0.30f;
static float rssiAvg = NAN;   // NAN until the first sample seeds it

void setup() {
  Fluxgrid.begin();
}

unsigned long lastSend = 0;

void loop() {
  Fluxgrid.run();             // keep the cloud link alive

  if (millis() - lastSend < SEND_INTERVAL) return;
  lastSend = millis();

  // WiFi.RSSI() is only meaningful while associated — skip the reading
  // otherwise so we don't publish a misleading 0 dBm during a reconnect.
  if (WiFi.status() != WL_CONNECTED) return;

  int raw = WiFi.RSSI();                          // e.g. -62 (dBm)
  rssiAvg = isnan(rssiAvg) ? raw : (1.0f - EMA_ALPHA) * rssiAvg + EMA_ALPHA * raw;

  Fluxgrid.write(RSSI_HANDLE, (int)lroundf(rssiAvg));  // publish the smoothed dBm
}
