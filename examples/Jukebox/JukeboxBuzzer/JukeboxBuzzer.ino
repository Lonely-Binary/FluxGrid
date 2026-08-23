/*
  Fluxgrid — Jukebox (play whole songs on a passive buzzer)

  The Jukebox widget streams an ENTIRE song to the device in one message: when
  you press play it writes the tune's notes as a single JSON payload to the Text
  "song" stream, plus a Boolean "play" (1 play / 0 stop). This sketch turns that
  payload into tone() calls on a passive buzzer — non-blocking, so the cloud
  link stays alive while a song plays.

  ── Why the FLUXGRID_MQTT_BUFFER line below ────────────────────────────────────
  A whole song is big. The library's MQTT buffer holds the COMPLETE packet
  (topic + framing + payload), and it defaults to 2048 bytes — fine for normal
  telemetry, but the longest songs (Star Wars, Doom, Bloody Tears, Für Elise…)
  serialize to ~3-4 KB and would be dropped. So we raise the buffer BEFORE the
  include. 8 KB leaves comfortable headroom for every song in the catalogue and
  costs only ~6 KB extra RAM (nothing on an ESP32):

      #define FLUXGRID_MQTT_BUFFER 1024*8   // 8 KB — must come before the include

  ── Payload format ─────────────────────────────────────────────────────────────
  { "fmt":"jbq.v1", "tempo":120, "m":[ midi, divider, midi, divider, … ] }
    • midi    — MIDI note number (0 = rest / silence)
    • divider — note length: 4 = quarter, 8 = eighth, 16 = sixteenth;
                negative = dotted (×1.5). Same convention as arduino-songs.
  Note length in ms = (60000*4 / tempo) / divider.

  Wiring:
    GPIO 25 → passive buzzer (+)        buzzer (−) → GND   (any PWM-capable pin)

  Dashboard setup:
    Drop a Jukebox widget (Control group). It auto-creates the two datastreams
    "song" (Text) and "play" (Boolean). Keep those handles, or rename them and
    update SONG_HANDLE / PLAY_HANDLE below.

  Songs come from the wonderful robsoncouto/arduino-songs project:
    https://github.com/robsoncouto/arduino-songs  — credit: Robson Couto.

  Requires: ESP32 Arduino core (tone() since 2.0.0) + ArduinoJson.
*/
#define FLUXGRID_MQTT_BUFFER 1024*8   // 8 KB — a whole song must fit one packet

#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← every #define above must come before this line
#include <ArduinoJson.h>

#define SONG_HANDLE "song"        // Text datastream — the streamed tune (jbq.v1)
#define PLAY_HANDLE "play"        // Boolean datastream — 1 play / 0 stop
#define BUZZER      25            // passive buzzer (+); other leg → GND

// MIDI note (0–127) → frequency in Hz. 69 = A4 = 440 Hz, 60 = C4.
static unsigned int midiToFreq(int m) {
  return (unsigned int)lroundf(440.0f * powf(2.0f, (m - 69) / 12.0f));
}

// The current song, parsed into a flat list of (midi, durationMs). midi 0 = rest.
static const int MAX_NOTES = 768;       // longest song in the catalogue is ~680 notes
static int16_t  gMidi[MAX_NOTES];
static uint16_t gMs[MAX_NOTES];
static int      gN = 0, gIdx = 0;
static bool     gPlaying = false, gOn = false;
static unsigned long gEndAt = 0, gOffAt = 0;

// Parse a {fmt,tempo,m} payload and arm playback from the first note.
static void loadSong(const String &json) {
  DynamicJsonDocument doc(FLUXGRID_MQTT_BUFFER);  // fits whatever the buffer allows
  if (deserializeJson(doc, json)) { FG_LOG("jukebox: bad payload"); return; }
  int tempo = doc["tempo"] | 120;
  float whole = (60000.0f * 4) / tempo;           // ms per whole note
  JsonArray m = doc["m"].as<JsonArray>();
  gN = 0;
  for (size_t i = 0; i + 1 < m.size() && gN < MAX_NOTES; i += 2) {
    int midi = m[i], divider = m[i + 1];
    float d = divider > 0 ? whole / divider : (whole / -divider) * 1.5f; // negative = dotted
    gMidi[gN] = (int16_t)midi;
    gMs[gN] = (uint16_t)d;
    gN++;
  }
  gIdx = 0; gOn = false; gPlaying = gN > 0; gEndAt = millis(); // first note fires now
  FG_LOG("jukebox: playing %d notes", gN);
}

// Advance playback by the wall clock without blocking loop().
static void serviceSong() {
  if (!gPlaying) return;
  unsigned long now = millis();
  if (gOn && now >= gOffAt) { noTone(BUZZER); gOn = false; }      // 90% gate → note articulation
  if (now >= gEndAt) {
    if (gIdx >= gN) { gPlaying = false; noTone(BUZZER); return; } // song finished
    int midi = gMidi[gIdx];
    unsigned long d = gMs[gIdx];
    gIdx++;
    if (midi > 0) { tone(BUZZER, midiToFreq(midi)); gOn = true; gOffAt = now + (d * 9) / 10; }
    else          { noTone(BUZZER); }                            // a rest
    gEndAt = now + d;
  }
}

void setup() {
  pinMode(BUZZER, OUTPUT);

  // A new song streamed in → start playing it.
  Fluxgrid.onReceive(SONG_HANDLE, [](FluxValue v) { loadSong(v.asString()); });

  // Play / stop button.
  Fluxgrid.onReceive(PLAY_HANDLE, [](FluxValue v) {
    if (v.asInt() == 0) { gPlaying = false; noTone(BUZZER); }     // stop
    else { gIdx = 0; gOn = false; gPlaying = gN > 0; gEndAt = millis(); } // restart current song
  });

  Fluxgrid.begin();              // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();                // keep the connection alive + dispatch writes
  serviceSong();                 // advance playback (non-blocking)
}
