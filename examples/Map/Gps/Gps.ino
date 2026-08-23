/*
  Gps.ino — plot a live GPS fix on a Fluxgrid Map widget (ESP32-S3 + GPS module).

  Unlike Map/IpLocation (coarse, IP-based, no hardware), this reads a real GPS
  receiver over UART and publishes a precise latitude/longitude to the Map widget
  every few seconds once it has a satellite fix — so the marker tracks where the
  board actually is, and movement draws a path on the map.

  Hardware
  ────────
    • ESP32-S3 dev board
    • Any NMEA GPS module (u-blox NEO-6M / NEO-M8N, etc.), 9600 baud 8N1
    • Wiring (UART2):
        GPS TX  → ESP32 GPIO16 (RX)
        GPS RX  → ESP32 GPIO17 (TX)
        GPS VCC → 3V3        GPS GND → GND
      Give the antenna a clear view of the sky. The first fix can take 1–2
      minutes outdoors (cold start); indoors you may never get one.

  Library
  ───────
  Install **TinyGPS++** (by Mikal Hart) from the Arduino Library Manager — it
  parses the raw NMEA stream coming off the module. The usual Fluxgrid deps still
  apply (ESP32 core + PubSubClient + ArduinoJson; ArduinoJson ships via
  Fluxgrid.h).

  The Map widget reads a single JSON datastream:
      {"lat":24.147,"lng":120.673,"acc":8,"label":"7 sats"}
  `acc` (accuracy radius in metres, derived from HDOP) and `label` are optional.

  Dashboard setup
  ───────────────
  1. Drop a Map widget on the canvas. Note the handle it creates (default "map").
  2. (Optional) Drop a Terminal widget bound to "log" to watch the raw fix.
  3. Flash this sketch with your credentials filled in below.
*/

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>               // ← credentials must be #defined ABOVE this line
#include <TinyGPS++.h>              // install "TinyGPS++" by Mikal Hart
#include <HardwareSerial.h>

#define MAP_HANDLE "map"            // must match the handle on your Map widget

// The GPS module on UART2. Most NMEA receivers default to 9600 baud, 8N1.
HardwareSerial GPS_Serial(2);       // UART2
TinyGPSPlus gps;                    // NMEA parser

static const uint32_t GPS_BAUD   = 9600;   // GPS module baud rate
static const int      GPS_RX_PIN = 16;     // ESP32 RX ← GPS TX
static const int      GPS_TX_PIN = 17;     // ESP32 TX → GPS RX
static const uint32_t PUBLISH_MS = 5000;   // throttle map updates to ~0.2 Hz

static uint32_t lastPublish = 0;

// Build the Map payload from the latest fix and publish it. HDOP (horizontal
// dilution of precision) is turned into a rough accuracy radius — ~5 m per HDOP
// unit is a sane figure for consumer receivers — so the widget draws an honest
// halo instead of implying a pinpoint.
void publishLocation() {
  StaticJsonDocument<192> out;
  out["lat"] = gps.location.lat();
  out["lng"] = gps.location.lng();          // note: lng, not lon
  if (gps.hdop.isValid()) {
    out["acc"] = (int)round(gps.hdop.hdop() * 5.0);
  }
  char label[20];
  snprintf(label, sizeof(label), "%lu sats", (unsigned long)gps.satellites.value());
  out["label"] = label;

  String payload;
  serializeJson(out, payload);
  Fluxgrid.write(MAP_HANDLE, payload);

  // Mirror the fix to the log datastream (Terminal widget) + USB monitor.
  Fluxgrid.print("Fix ");       Fluxgrid.print(gps.location.lat(), 6);
  Fluxgrid.print(", ");         Fluxgrid.print(gps.location.lng(), 6);
  Fluxgrid.print(" · sats ");   Fluxgrid.print(gps.satellites.value());
  Fluxgrid.print(" · ");        Fluxgrid.print(gps.speed.kmph(), 1);
  Fluxgrid.println(" km/h");
}

void setup() {
  GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Fluxgrid.begin();                         // WiFi + cloud — TLS built in
}

void loop() {
  Fluxgrid.run();                           // keep the connection alive

  // Drain the GPS serial buffer and feed every byte to the parser.
  while (GPS_Serial.available() > 0) gps.encode(GPS_Serial.read());

  // Publish at most once per PUBLISH_MS, and only on a fresh, valid fix.
  if (gps.location.isValid() && gps.location.isUpdated() &&
      millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();
    publishLocation();
  }

  // No bytes at all after a few seconds usually means a wiring / baud problem.
  // Warn once so we don't spam the log while waiting for a fix.
  static bool warned = false;
  if (!warned && millis() > 5000 && gps.charsProcessed() < 10) {
    warned = true;
    Fluxgrid.println("No GPS data — check wiring (GPS TX→16, RX→17), baud, antenna.");
  }
}
