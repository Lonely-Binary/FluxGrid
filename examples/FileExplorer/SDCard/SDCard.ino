/*
  FileExplorerSD — browse an SD card from the dashboard

  Exposes an SD card (FATFS) to the Fluxgrid File Explorer widget: browse
  folders, download / upload files, rename and delete — all from the browser.
  The library handles the wire protocol; you mount the card and register it.

  (For the on-chip flash version, see File ▸ Examples ▸ Fluxgrid ▸ FileExplorer.)

  WHO OWNS WHAT
    YOU mount the card. The library never touches the SD bus or pins — bring it
    up yourself so you stay in full control of the bus mode (SDMMC vs SPI) and
    the GPIOs. Pick ONE of the two options below to match your wiring.

  FORMAT
    SD cards have no portable "format", so the dashboard's Format button on an SD
    volume DELETES EVERYTHING AT THE ROOT instead (a full wipe), rather than
    re-creating the filesystem.

  DASHBOARD
    Drop a "File Explorer" widget on the canvas and point it at this device.
    Bulk download/upload needs object storage (MinIO) configured for your
    workspace; browsing / rename / delete / mkdir work without it.
*/

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>

// ── Choose ONE SD bus ────────────────────────────────────────────────────────
// Option A: SDMMC (faster, 1- or 4-bit). On the classic ESP32 the pins are
// fixed; on the ESP32-S3 they're remappable via SD_MMC.setPins(...) before
// begin(). This is the default below.
#include <SD_MMC.h>
// Option B: SPI. Comment out <SD_MMC.h> above, uncomment the two lines below,
// and use the SD.begin(CS) block in setup() instead.
// #include <SPI.h>
// #include <SD.h>

void setup() {
  Serial.begin(115200);
  delay(3000);
  
  // ── Option A: SDMMC ────────────────────────────────────────────────────────
  // On the ESP32-S3 set your slot pins first, e.g.:
  //   SD_MMC.setPins(/*clk=*/14, /*cmd=*/15, /*d0=*/2 /*, d1,d2,d3 for 4-bit*/);
  if (!SD_MMC.begin()) {
    Serial.println("SD_MMC mount failed — check wiring / card");
  } else {
    // Style "B": global object + known type → capacity auto-detected.
    Fluxgrid.addVolume("sd", SD_MMC, FG_SD);
  }

  // ── Option B: SPI (use instead of Option A) ────────────────────────────────
  //   const int SD_CS = 5;                       // your chip-select pin
  //   if (!SD.begin(SD_CS)) {
  //     Serial.println("SD (SPI) mount failed — check wiring / card");
  //   } else {
  //     Fluxgrid.addVolume("sd", SD, FG_SD);
  //   }

  // Turn the file explorer on (subscribe to the fs command channel), then connect.
  Fluxgrid.enableFiles();
  Fluxgrid.begin();
}

void loop() {
  Fluxgrid.run();   // services metadata commands + publishes transfer progress
}
