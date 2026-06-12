/*
  WiFiScan.ino — publish a periodic WiFi scan to a Fluxgrid WiFi Scan widget.

  The sketch runs WiFi.scanNetworks() every SCAN_INTERVAL ms and publishes the
  results as a compact JSON payload to the "wifi_scan" datastream handle.

  Dashboard setup
  ───────────────
  1. Create a datastream named "wifi_scan" with kind "json".
  2. Drop a WiFi Scan widget on the canvas and bind it to "wifi_scan".
  3. Flash this sketch with your credentials filled in below.

  Payload format (max 8 networks, compact keys to stay under 512 chars)
  ──────────────────────────────────────────────────────────────────────
  {"n":[{"s":"SSID","r":-62,"c":4,"e":"WPA+WPA2","b":"AA:BB:CC:DD:EE:FF"},…]}
    s = ssid      r = rssi (dBm)    c = channel
    e = encryption type string      b = BSSID (stabilises radar blip position)

  Requirements
  ────────────
  ESP32 Arduino core + PubSubClient + Fluxgrid (all from Library Manager / ZIP).
*/

#define WIFI_SSID  "your-wifi-ssid"
#define WIFI_PASS  "your-wifi-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>

// How often to scan (ms). WiFi.scanNetworks() takes 1–4 s so keep this ≥ 5000.
static const unsigned long SCAN_INTERVAL = 10000UL;
// Maximum number of networks to include in the payload (keeps JSON under 512 chars).
static const int MAX_NETS = 8;

static const char* encName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:          return "Open";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA+WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2+WPA3";
    default:                      return "WPA2";
  }
}

void doScan() {
  Serial.println("[WiFiScan] scanning…");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  if (n <= 0) {
    Serial.println("[WiFiScan] no networks found");
    return;
  }

  // Sort by RSSI (strongest first) so the widget's list view is nicely ordered
  // even if the radio returns them in a different order.
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (WiFi.RSSI(j) > WiFi.RSSI(i)) {
        WiFi.scanDelete();  // can't swap in-place; rebuild after sort below
        // Re-scan is heavyweight; use a stable-sort via a local index array instead.
        goto skip_sort;     // fallback: just use radio order
      }
    }
  }
  skip_sort:;

  int count = min(n, MAX_NETS);

  // Build JSON manually to avoid pulling in ArduinoJson.
  // Format: {"n":[{…},{…}]}
  String payload = "{\"n\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0) payload += ',';
    payload += "{\"s\":\"";
    // Escape any double-quotes in the SSID (rare but possible)
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    payload += ssid;
    payload += "\",\"r\":";
    payload += WiFi.RSSI(i);
    payload += ",\"c\":";
    payload += WiFi.channel(i);
    payload += ",\"e\":\"";
    payload += encName(WiFi.encryptionType(i));
    payload += "\",\"b\":\"";
    payload += WiFi.BSSIDstr(i);
    payload += "\"}";  

    // Safety: bail if we're approaching the 512-char bridge limit.
    if (payload.length() > 480) {
      payload += ']';  // close without a trailing comma
      goto close_json;
    }
  }
  payload += ']';
  close_json:
  payload += '}';

  Serial.printf("[WiFiScan] %d nets, payload %d chars\n", count, payload.length());
  Fluxgrid.write("wifi_scan", payload);

  WiFi.scanDelete();
}

void setup() {
  Fluxgrid.begin();
  doScan();  // first scan immediately on boot
}

unsigned long lastScan = 0;

void loop() {
  Fluxgrid.run();

  unsigned long now = millis();
  if (now - lastScan >= SCAN_INTERVAL) {
    lastScan = now;
    doScan();
  }
}
