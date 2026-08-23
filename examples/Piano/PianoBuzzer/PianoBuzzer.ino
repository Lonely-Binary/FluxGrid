/*
  Fluxgrid — Piano (play a passive buzzer from the Piano widget)

  The Piano widget on your dashboard sends two things, on two datastreams:

    • note (Number) — the live MIDI note as you press a key (0 = silence).
                      We play it on a passive buzzer the instant it arrives.
    • seq  (Text)   — on SEND, the whole recorded take as one "mseq.v1" JSON
                      string ({pin,fmt,dur,count,events}); we parse it and
                      schedule each note so the buzzer replays the performance.

  A passive buzzer needs an AC waveform (not just HIGH), so we drive it with
  tone()/noTone() — the ESP32 Arduino core implements these on top of LEDC.
  The buzzer is monophonic (one voice), so the newest held note wins; the widget
  already applies last-note priority on the live "note" stream, and the replay
  below tracks the sounding note so a stale note-off never cuts a newer note.

  Wiring:
    GPIO 25 → passive buzzer (+)        buzzer (−) → GND
    (any PWM-capable GPIO works; avoid the strapping pins)

  Dashboard setup:
    Drop a Piano widget (Control group). It auto-creates the two datastreams
    "note" (Number) and "seq" (Text). Keep those handles — or rename them and
    update NOTE_HANDLE / SEQ_HANDLE below to match.

  Note on size: a recorded take is sent as one MQTT message, so keep takes
  modest — the payload must fit the library's 2 KB MQTT buffer (a busy 30 s of
  playing can exceed it). Short melodies are well within budget.

  Requires: ESP32 Arduino core (tone() since 2.0.0) + ArduinoJson.
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line
#include <ArduinoJson.h>

#define NOTE_HANDLE "note"        // Number datastream — live MIDI note (0 = silence)
#define SEQ_HANDLE  "seq"         // Text datastream   — the recorded take (mseq.v1)
#define BUZZER      25            // passive buzzer (+); other leg → GND

// MIDI note (0–127) → frequency in Hz. 69 = A4 = 440 Hz, 60 = C4.
static unsigned int midiToFreq(int m) {
  return (unsigned int)lroundf(440.0f * powf(2.0f, (m - 69) / 12.0f));
}

// Monophonic buzzer: play a MIDI note now, or silence it on 0 / note-off.
static int curNote = -1;
static void buzz(int m) {
  if (m > 0) { tone(BUZZER, midiToFreq(m)); curNote = m; }
  else       { noTone(BUZZER);              curNote = -1; }
}

// --- the recorded take, replayed without blocking loop() ---
struct Ev { uint8_t midi; uint8_t on; uint32_t t; };  // [note, on/off, ms offset]
static const int MAX_EV = 240;
static Ev       gSeq[MAX_EV];
static int      gLen = 0, gIdx = 0;
static uint32_t gStart = 0;
static bool     gPlaying = false;

// Parse the mseq.v1 JSON the widget sends on SEND and arm playback.
static void loadSeq(const String &json) {
  DynamicJsonDocument doc(4096);          // a take fits inside the 2 KB MQTT buffer
  if (deserializeJson(doc, json)) return; // ignore malformed payloads
  JsonArray evs = doc["events"].as<JsonArray>();
  gLen = 0;
  for (JsonArray e : evs) {
    if (gLen >= MAX_EV) break;
    gSeq[gLen].midi = (uint8_t)(int)e[0];
    gSeq[gLen].on   = (uint8_t)(int)e[1];
    gSeq[gLen].t    = (uint32_t)(long)e[2];
    gLen++;
  }
  gIdx = 0;
  gStart = millis();
  gPlaying = (gLen > 0);
  FG_LOG("piano: replaying %d events", gLen);
}

// Fire any events whose offset has arrived (called every loop()).
static void serviceSeq() {
  if (!gPlaying) return;
  uint32_t el = millis() - gStart;
  while (gIdx < gLen && gSeq[gIdx].t <= el) {
    Ev &e = gSeq[gIdx++];
    if (e.on)                   buzz(e.midi); // note-on  → play
    else if (e.midi == curNote) buzz(0);      // note-off → silence (monophonic)
  }
  if (gIdx >= gLen) { buzz(0); gPlaying = false; } // take finished
}

void setup() {
  pinMode(BUZZER, OUTPUT);

  // Live key press → play that note on the buzzer immediately.
  Fluxgrid.onReceive(NOTE_HANDLE, [](FluxValue v) { buzz(v.asInt()); });

  // SEND → replay the whole recorded take with its original timing.
  Fluxgrid.onReceive(SEQ_HANDLE, [](FluxValue v) { loadSeq(v.asString()); });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive + dispatch writes
  serviceSeq();                  // advance any recorded playback (non-blocking)
}
