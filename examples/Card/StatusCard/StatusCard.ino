/*
  Fluxgrid — Card (a TEXT status word in a configurable value Card)

  Unlike a Gauge, a Card can display a *text* datastream — so it's perfect for a
  one-word machine state: "OK", "WARN", "FAULT". You write() a string instead of
  a number, and the conditional-colour rule reacts to the word rather than a
  threshold.

  This sketch derives a status from a sensor reading and publishes the word.

  Dashboard setup:
    1. Create a datastream named "status" with kind "text".
       (On a text stream the Card uses no postfix and the condition compares
        with contains / equals instead of > / <.)
    2. Drop a Card widget and bind it to "status".
    3. In the Properties panel:
         • Icon → "activity"
         • Conditional icon color → when value equals "FAULT", On color = red
           (leave Off color green/neutral for OK/WARN)
    4. (Optional) "Save as template" → "Status" so the look is reusable.

  Wiring:
    Replace the analogRead() below with whatever you're monitoring (current
    sensor, door switch, tank level, ...).
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

#define STATUS_HANDLE "status"   // must match your Card widget's datastream handle

static const unsigned long SEND_INTERVAL = 2000UL;

// Thresholds (in raw 12-bit ADC counts) that split the reading into states.
static const int WARN_LEVEL  = 2600;
static const int FAULT_LEVEL = 3500;

// Map a reading to a status word. Returning a const char* keeps it allocation-free.
static const char *statusFor(int reading) {
  if (reading >= FAULT_LEVEL) return "FAULT";
  if (reading >= WARN_LEVEL)  return "WARN";
  return "OK";
}

void setup() {
  Fluxgrid.begin();             // server, port and TLS are built in
}

unsigned long lastSend = 0;
const char *lastStatus = nullptr;

void loop() {
  Fluxgrid.run();               // keep the connection alive

  if (millis() - lastSend < SEND_INTERVAL) return;
  lastSend = millis();

  int reading = analogRead(4);          // swap in your real signal
  const char *status = statusFor(reading);

  // Publish only when the word changes — text rarely needs a steady stream, and
  // the Card keeps showing the last value it received.
  if (status != lastStatus) {
    lastStatus = status;
    Fluxgrid.write(STATUS_HANDLE, status);   // e.g. the Card shows "FAULT" in red
  }
}
