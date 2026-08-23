/*
  MemoryMonitor.ino — publish the ESP32's heap usage to a Fluxgrid Memory widget.

  Pairs with the dashboard "Memory" widget. The widget reads three datastreams —
  bind them to the three values this sketch writes:

    free_heap      ESP.getFreeHeap()     bytes free right now
    min_free_heap  ESP.getMinFreeHeap()  low-water mark since boot (the worst dip)
    max_alloc      ESP.getMaxAllocHeap()  largest single block still allocatable

  Watching free_heap trend down while max_alloc shrinks faster is the classic
  fingerprint of heap fragmentation — handy for catching a slow leak in the field.

  Dashboard setup
  ───────────────
  1. Create three datastreams (kind "number"): "free_heap", "min_free_heap",
     "max_alloc".
  2. Drop a Memory widget on the canvas and bind its three slots to them.
  3. Flash this sketch with your credentials filled in below.

  Requirements
  ────────────
  ESP32 Arduino core + PubSubClient + Fluxgrid (all from Library Manager / ZIP).
*/

#define WIFI_SSID  "your-wifi-ssid"
#define WIFI_PASS  "your-wifi-password"
#define FG_TOKEN   "DEVICE_TOKEN"   // from dashboard → Pair device
#include <Fluxgrid.h>

// Datastream handles — must match the Memory widget's three bound datastreams.
#define FREE_HEAP_HANDLE     "free_heap"
#define MIN_FREE_HEAP_HANDLE "min_free_heap"
#define MAX_ALLOC_HANDLE     "max_alloc"

// How often to publish (ms). Heap moves slowly; 2 s keeps the graph live without
// spamming the broker.
static const unsigned long SEND_INTERVAL = 2000UL;

void setup() {
  Fluxgrid.begin();
}

unsigned long lastSend = 0;

void loop() {
  Fluxgrid.run();             // keep the cloud link alive

  if (millis() - lastSend < SEND_INTERVAL) return;   // non-blocking throttle
  lastSend = millis();

  Fluxgrid.write(FREE_HEAP_HANDLE,     ESP.getFreeHeap());      // free right now
  Fluxgrid.write(MIN_FREE_HEAP_HANDLE, ESP.getMinFreeHeap());   // low-water mark
  Fluxgrid.write(MAX_ALLOC_HANDLE,     ESP.getMaxAllocHeap());  // largest block
}
