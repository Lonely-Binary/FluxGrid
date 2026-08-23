/*
  LocalDashboardCloud — receive a pushed dashboard from the cloud, then serve it
  offline from the device's own WiFi access point.

  This is the AP+STA companion to the LocalDashboard example. The device joins
  your WiFi (STA) so the Fluxgrid app can reach it — that's how "Push to device"
  writes the dashboard onto the on-chip flash — AND runs its own access point
  (AP) so a phone/laptop can open the dashboard offline at http://192.168.4.1/
  afterwards, even with no internet.

  Flow:
    1. Flash this once (set your WiFi + device token below).
    2. In the Fluxgrid editor: Manage ▸ "Push to device…" while the device is
       online. The page's layout is written to LittleFS /dashboard.json.
    3. Connect a phone to the "Fluxgrid-Device" hotspot and open the dashboard —
       it renders offline. Telemetry you write() shows live; taps fire onReceive().

  Why the extra calls vs. LocalDashboard:
    • addVolume()/enableFiles() expose LittleFS so the cloud push can write to it.
    • cloud(true) (the default) keeps the MQTT link up to receive the push.
    (Plain LocalDashboard is cloud(false), AP-only — great for pure-offline use,
     but it cannot receive a cloud push.)

  Requires (install via Library Manager): ESPAsyncWebServer, AsyncTCP, ArduinoJson.
  Pick a Partition Scheme with a LittleFS/SPIFFS partition (Tools ▸ Partition
  Scheme) — the pushed dashboard (and player bundle) live there.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // from the dashboard — needed for the cloud push
#include <Fluxgrid.h>
#include <FluxgridLocal.h>
#include <WiFi.h>
#include <LittleFS.h>

#define TEMP_HANDLE  "temp"
#define RELAY_HANDLE "relay"

void setup() {
  Serial.begin(115200);

  // The sketch owns WiFi so we can run STA (cloud) and AP (offline) together.
  Fluxgrid.manageWiFi(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Fluxgrid-Device", "fluxgrid123");   // offline dashboard hotspot
  WiFi.begin(WIFI_SSID, WIFI_PASS);                 // join your WiFi for the cloud
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  // Mount flash and expose it so the cloud "Push to device" can write /dashboard.json.
  LittleFS.begin(true);
  Fluxgrid.addVolume("flash", LittleFS, FG_FLASH);
  Fluxgrid.enableFiles();               // subscribe to the fs command channel

  Fluxgrid.cloud(true);                 // stay on the cloud (default) to receive pushes
  Fluxgrid.begin();

  // React to control widgets locally (same callback as cloud control writes).
  Fluxgrid.onReceive(RELAY_HANDLE, [](FluxValue v) { digitalWrite(2, v.asBool()); });

  FluxgridLocal.startWeb();             // http://192.168.4.1/ (and your LAN IP)
  Serial.println(FluxgridLocal.url());
}

void loop() {
  Fluxgrid.run();                       // cloud MQTT + fs transfers (the push)
  FluxgridLocal.loop();                 // pumps the captive-portal DNS
  Fluxgrid.write(TEMP_HANDLE, analogRead(4) * (50.0 / 4095.0));  // live on the dashboard
  delay(2000);
}
