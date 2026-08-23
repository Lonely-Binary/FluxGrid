/*
  LocalDashboard — serve a Fluxgrid dashboard from the ESP32's own WiFi access
  point, with no cloud. Connect a phone/laptop to the device's hotspot and open
  http://192.168.4.1/ — the dashboard runs fully offline.

  Push the dashboard onto the device first from the Fluxgrid app ("Push to
  device"), or drop the files via the onboarding page. Telemetry you write()
  shows live; widgets you tap there fire your onReceive().

  Requires (install via Library Manager): ESPAsyncWebServer, AsyncTCP,
  ArduinoJson. Pick a Partition Scheme with a LittleFS/SPIFFS partition
  (Tools ▸ Partition Scheme).
*/
#define FG_TOKEN "DEVICE_TOKEN"        // only needed if you also use the cloud
#include <Fluxgrid.h>
#include <FluxgridLocal.h>
#include <WiFi.h>

#define TEMP_HANDLE  "temp"
#define RELAY_HANDLE "relay"

void setup() {
  Serial.begin(115200);

  // The sketch owns WiFi: bring up the device's own access point.
  Fluxgrid.manageWiFi(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Fluxgrid-Device", "fluxgrid123");

  Fluxgrid.cloud(false);               // pure local — no broker. (Drop this and
  Fluxgrid.begin();                    // join WiFi to ALSO go to the cloud.)

  // React to control widgets locally (same callback as cloud control writes).
  Fluxgrid.onReceive(RELAY_HANDLE, [](FluxValue v) { digitalWrite(2, v.asBool()); });

  FluxgridLocal.startWeb();             // http://192.168.4.1/
  Serial.println(FluxgridLocal.url());
}

void loop() {
  Fluxgrid.run();
  FluxgridLocal.loop();                 // pumps the captive-portal DNS
  Fluxgrid.write(TEMP_HANDLE, analogRead(4) * (50.0 / 4095.0));  // live on the dashboard
  delay(2000);
}
