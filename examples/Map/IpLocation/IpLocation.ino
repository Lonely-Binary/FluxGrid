/*
  IpLocation.ino — show the device's approximate location on a Fluxgrid Map
  widget, derived from its public IP address (no GPS hardware required).

  On connect, the device asks ip-api.com where its public IP lives and publishes
  the result to a Map widget. IP geolocation is coarse — city-level at best, and
  plain wrong behind a VPN / mobile carrier — so this sketch is deliberately
  honest about that:

    • Layer A (data)    — the latitude/longitude are rounded to ~1 km before
                          sending, so we never publish a deceptively precise pin.
    • Layer B (display) — an `acc` (accuracy radius, in metres) is sent alongside
                          lat/lng; the Map widget draws a halo of that size
                          ("somewhere in here") instead of a pinpoint marker.

  The Map widget reads a single JSON datastream:
      {"lat":24.06,"lng":120.55,"acc":25000,"label":"Taichung"}
  `acc` and `label` (marker popup text) are optional.

  Dashboard setup
  ───────────────
  1. Drop a Map widget on the canvas. Note the handle it creates (default "map").
  2. (Optional) Drop a Terminal widget bound to "log" to see the IP / city / ISP.
  3. Flash this sketch with your credentials filled in below.

  ip-api.com notes
  ────────────────
    • Free tier is HTTP-only (HTTPS needs a paid key) and non-commercial use.
    • Rate-limited to 45 requests/minute per IP. Your public IP barely changes,
      so we fetch ONCE per cloud (re)connect via onConnected() — no polling.

  Requirements
  ────────────
  ESP32 Arduino core + PubSubClient + ArduinoJson + Fluxgrid. HTTPClient ships
  with the ESP32 core; ArduinoJson is already pulled in by <Fluxgrid.h>.
*/

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>               // ← credentials must be #defined ABOVE this line
#include <HTTPClient.h>             // ESP32 core; ArduinoJson comes via Fluxgrid.h

#define MAP_HANDLE "map"            // must match the handle on your Map widget

// Accuracy radius (metres) reported with the position. IP geolocation lands you
// in roughly the right city, so a ~25 km halo is an honest "around here".
static const int IP_ACCURACY_M = 25000;

// Look up the public IP's location and publish it to the Map widget. Called once
// each time the cloud link comes up (registered with onConnected, below).
void publishLocation() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=status,query,country,city,lat,lon,isp");
  int code = http.GET();             // blocks briefly; fine — we run this rarely
  if (code == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString()) && doc["status"] == "success") {
      double lat = doc["lat"] | 0.0;
      double lon = doc["lon"] | 0.0;             // ip-api calls it "lon"
      const char *city = doc["city"]  | "";
      const char *ip   = doc["query"] | "";
      const char *isp  = doc["isp"]   | "";

      // Build the Map payload. Round to 2 dp (~1.1 km) so the coordinate itself
      // is no more precise than the IP lookup actually is. Note lon → "lng".
      double mapLat = round(lat * 100) / 100.0;
      double mapLng = round(lon * 100) / 100.0;
      StaticJsonDocument<192> out;
      out["lat"]   = mapLat;
      out["lng"]   = mapLng;
      out["acc"]   = IP_ACCURACY_M;
      out["label"] = city;
      String payload;
      serializeJson(out, payload);
      Fluxgrid.write(MAP_HANDLE, payload);

      // Bonus: mirror the details to the log datastream (Terminal widget) + USB.
      Fluxgrid.print("Public IP "); Fluxgrid.print(ip);
      Fluxgrid.print(" · ");        Fluxgrid.print(city);
      Fluxgrid.print(" · ");        Fluxgrid.println(isp);
      // Also print the published position (lat, lng ± accuracy in km) so the
      // coordinates are visible in the Terminal / Serial Monitor, not just on
      // the map. 6 dp prints the full rounded value without trailing noise.
      Fluxgrid.print("Location ");  Fluxgrid.print(mapLat, 6);
      Fluxgrid.print(", ");         Fluxgrid.print(mapLng, 6);
      Fluxgrid.print(" (±");        Fluxgrid.print(IP_ACCURACY_M / 1000);
      Fluxgrid.println(" km)");
    } else {
      Fluxgrid.println("ip-api: lookup failed");
    }
  } else {
    Fluxgrid.print("ip-api: HTTP "); Fluxgrid.println(code);
  }
  http.end();
}

void setup() {
  Fluxgrid.onConnected(publishLocation); // fetch once whenever the cloud link is up
  Fluxgrid.begin();                      // WiFi + cloud — TLS built in
}

void loop() {
  Fluxgrid.run();                        // keep the connection alive
}
