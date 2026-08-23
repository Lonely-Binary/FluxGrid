/*
  FileExplorer — browse the on-chip flash (LittleFS) from the dashboard

  Exposes the ESP32's internal LittleFS filesystem to the Fluxgrid File Explorer
  widget: browse folders, download / upload files, rename, delete and format —
  all from the browser. The library handles the wire protocol; you just mount the
  filesystem and register it as a volume.

  (For an SD card example, see File ▸ Examples ▸ Fluxgrid ▸ FileExplorerSD.)

  DASHBOARD
    Drop a "File Explorer" widget on the canvas and point it at this device.
    Bulk download/upload needs object storage (MinIO) configured for your
    workspace; browsing / rename / delete / mkdir / format work without it.

  PARTITIONS
    Pick a Partition Scheme that includes a filesystem partition in
    Tools ▸ Partition Scheme (e.g. "Default 4MB with spiffs", or any scheme with
    a SPIFFS/LittleFS region). LittleFS.begin(true) formats it on first use.
*/

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>

#include <LittleFS.h>     // on-chip flash filesystem (always available)

void setup() {
  Serial.begin(115200);
  delay(3000);
  // Mount the on-chip flash filesystem. begin(true) formats it the first time
  // if it isn't formatted yet.
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — check your Partition Scheme");
  } else {
    // Leave a sample file behind so the explorer has something to show.
    File f = LittleFS.open("/hello.txt", FILE_WRITE);
    if (f) { f.println("Hello from Fluxgrid!"); f.close(); }

    // Register it. Style "B": pass the global object + a known type and the
    // library auto-detects capacity (total / used). FG_FLASH wires up a real
    // .format() for the dashboard's Format button.
    Fluxgrid.addVolume("flash", LittleFS, FG_FLASH);
  }

  // Turn the file explorer on (subscribe to the fs command channel), then connect.
  Fluxgrid.enableFiles();
  Fluxgrid.begin();
}

void loop() {
  Fluxgrid.run();   // services metadata commands + publishes transfer progress
}
