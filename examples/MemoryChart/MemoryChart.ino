/*
  MemoryChart.ino — one-line automatic memory reporting for the Fluxgrid
  Memory Chart widget.

  Fluxgrid.reportMemory() is all you need: the library publishes the ESP32's
  heap (and PSRAM, when the board has it) to the cloud every few seconds on its
  own — no per-loop write() calls, and no datastreams to create or bind. All the
  numbers ride in ONE MQTT message, and the widget reads them straight off the
  device.

  What gets published each tick (heap always; PSRAM only on boards that have it):
    free_heap       ESP.getFreeHeap()      heap bytes free right now
    min_free_heap   ESP.getMinFreeHeap()   heap low-water mark since boot
    max_alloc       ESP.getMaxAllocHeap()  largest single block still allocatable
    free_psram      ESP.getFreePsram()     PSRAM bytes free right now
    min_free_psram  ESP.getMinFreePsram()  PSRAM low-water mark since boot

  The fixed *totals* (heap size, PSRAM size) come automatically from the device
  info the library already reports on connect, so there is nothing else to send.

  Dashboard setup
  ───────────────
  1. Drop a Memory Chart widget on the canvas.
  2. In its Properties panel, pick this device (just like the Device Info widget)
     — no datastreams to wire up.
  3. Pick the style (Fluxgrid / Swiss / Phosphor / Fluid).
  4. Flash this sketch with your credentials filled in below.

  Requirements
  ────────────
  ESP32 Arduino core + PubSubClient + Fluxgrid (all from Library Manager / ZIP).
*/

#define WIFI_SSID  "your-wifi-ssid"
#define WIFI_PASS  "your-wifi-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>

void setup() {
  // One line turns on automatic memory reporting. The default cadence is 5 s;
  // pass a value for a snappier demo (Fluxgrid.reportMemory(2000)) or a gentler
  // long-term leak watch (Fluxgrid.reportMemory(30000)).
  Fluxgrid.reportMemory();
  Fluxgrid.begin();
}

void loop() {
  Fluxgrid.run();   // keeps the cloud link alive and publishes the snapshots
}
