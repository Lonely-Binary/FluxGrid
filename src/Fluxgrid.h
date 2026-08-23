/*
  Fluxgrid — the simple ESP32 client for the Fluxgrid IoT platform.

  Complex inside (WiFi, MQTT, reconnect, Last-Will, topic plumbing),
  simple outside. A full device is ~10 lines:

      #define WIFI_SSID  "wifi"
      #define WIFI_PASS  "pass"
      #define FG_TOKEN   "DEVICE_TOKEN"   // one string per device, from the dashboard
      #include <Fluxgrid.h>

      void setup() {
        Fluxgrid.begin();   // server, port and TLS are built in
      }

      void loop() {
        Fluxgrid.run();
        Fluxgrid.write("temp", analogRead(4) * (50.0 / 4095.0));  // push a number
        if (Fluxgrid.read("pump").asBool()) digitalWrite(26, HIGH); // read it back
      }

      // Event style (optional) — fires once per incoming write. read() returns a
      // FluxValue you convert explicitly: .asInt() .asFloat() .asBool() .asString()
      // Fluxgrid.onReceive("pump", [](FluxValue v){ digitalWrite(26, v.asBool()); });

  Get the single device token from your dashboard → "Pair device". It packs the
  routing token + per-device MQTT user/pass as "<token>.<user>.<pass>"; the
  library splits it for you, so one device = one token.
  Wire protocol (handled for you):
      fluxgrid/<token>/v/<pin>   telemetry up
      fluxgrid/<token>/w/<pin>   control writes down  (retained)
      fluxgrid/<token>/status    online/offline (retained, via MQTT LWT)
      fluxgrid/<token>/meta      device info {"ssid":…,"info":{chip,mem,mac,fw,net}} (retained, on connect)

  Requires: ESP32 Arduino core + PubSubClient (knolleary).
  MIT License · Lonely Binary
*/
#ifndef FLUXGRID_H
#define FLUXGRID_H

/*
  Library version — keep in sync with library.properties `version=`.

  Printed to the debug log on boot (so you can confirm which version is flashed
  from the Serial Monitor) and reported to the dashboard on connect, so the
  cloud can show each device's running version and flag out-of-date firmware.
*/
#ifndef FLUXGRID_VERSION
#define FLUXGRID_VERSION "0.18.0"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <functional>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <FS.h>   // fs::FS base — part of the ESP32 core, always present

#ifndef FLUXGRID_MAX_INPUTS
#define FLUXGRID_MAX_INPUTS  16
#endif
#ifndef FLUXGRID_MAX_OUTPUTS
#define FLUXGRID_MAX_OUTPUTS 8
#endif
#ifndef FLUXGRID_MAX_PWM
#define FLUXGRID_MAX_PWM     8
#endif

#ifndef FLUXGRID_MAX_SLOTS
#define FLUXGRID_MAX_SLOTS 32
#endif

/*
  MQTT packet buffer (bytes). The WHOLE packet — topic + framing + payload —
  must fit here, for both what you publish and what you receive. The default
  2048 holds telemetry, control writes and a ~16-network WiFi scan.

  Raise it BEFORE #include <Fluxgrid.h> when a widget sends or receives a larger
  payload — e.g. the Jukebox streams an entire song in one message:

      #define FLUXGRID_MQTT_BUFFER 1024*8   // 8 KB
      #include <Fluxgrid.h>

  Costs that many bytes of RAM (trivial on an ESP32). Applied in begin() via
  PubSubClient::setBufferSize().
*/
#ifndef FLUXGRID_MQTT_BUFFER
#define FLUXGRID_MQTT_BUFFER 2048
#endif

/*
  File explorer — how many mounted volumes a device can expose at once.

  Each addVolume() call (one per mounted fs::FS, e.g. LittleFS + an SD card)
  takes one slot. Raise this BEFORE #include <Fluxgrid.h> if a board mounts more.
*/
#ifndef FLUXGRID_MAX_VOLUMES
#define FLUXGRID_MAX_VOLUMES 4
#endif

/*
  Volume type constants for addVolume(). They classify a mounted filesystem so
  the dashboard can pick the right icon and the library knows how to "format" it:

    FG_FLASH   on-chip flash (LittleFS / FFat) — real .format() available
    FG_SD      removable SD card (SD / SD_MMC) — no portable format (wipe-all)
    FG_FS_OTHER  anything else mounted as an fs::FS

  Reported to the cloud as the strings "flash" / "sd" / "other".
*/
#define FG_FLASH    ((uint8_t)0)
#define FG_SD       ((uint8_t)1)
#define FG_FS_OTHER ((uint8_t)2)

/*
  Compile-time "does T have a format() method?" used by the templated
  addVolume() for known flash types. Only LittleFS / FFat expose format(); the
  SD classes (SD / SD_MMC) do not. The first overload is selected (via the
  preferred `int` argument) only when `p->format()` is well-formed, so the
  template instantiates cleanly even for an SD filesystem — it just gets a null
  format function and a "format" request falls back to wiping the volume.
*/
template <typename T>
inline auto _fgMakeFormatFn(T *p, int) -> decltype(p->format(), std::function<bool()>()) {
  return [p]() -> bool { return p->format(); };
}
template <typename T>
inline std::function<bool()> _fgMakeFormatFn(T * /*p*/, long) {
  return std::function<bool()>(); // T has no format() — null
}

/*
  Debug logging — ON by default.

  The library narrates what it's doing (WiFi join, cloud connect, every value
  sent/received, config applied) to the serial monitor so you can see exactly
  what's happening while you build. Open Tools ▸ Serial Monitor at 115200 baud.

  Turn it OFF by #defining FLUXGRID_DEBUG to 0 BEFORE #include <Fluxgrid.h>:

      #define FLUXGRID_DEBUG 0
      #include <Fluxgrid.h>

  Lines are prefixed with "[Fluxgrid]". When debug is on, begin() also starts
  Serial for you (at FLUXGRID_DEBUG_BAUD) if you haven't already.
*/
#ifndef FLUXGRID_DEBUG
#define FLUXGRID_DEBUG 1
#endif
#ifndef FLUXGRID_DEBUG_BAUD
#define FLUXGRID_DEBUG_BAUD 115200
#endif

/*
  Serial capture — show the device's serial output on the dashboard.

  Set this to 1 BEFORE #include <Fluxgrid.h> and your existing Serial.print /
  Serial.println calls are tee'd to BOTH the USB Serial Monitor and a Fluxgrid
  "log" datastream — drop a Terminal widget on the canvas and it shows the same
  lines. No other code change:

      #define FLUXGRID_CAPTURE_SERIAL 1
      #include <Fluxgrid.h>
      ...
      Serial.println("hello");   // → USB + dashboard

  Left at 0 (the default), Serial is the normal Arduino HardwareSerial, fully
  untouched. For the same effect without redefining Serial, call
  Fluxgrid.println(...) explicitly instead (see below).
*/
#ifndef FLUXGRID_CAPTURE_SERIAL
#define FLUXGRID_CAPTURE_SERIAL 0
#endif

#if FLUXGRID_DEBUG
  #define FG_LOG(fmt, ...) Serial.printf("[Fluxgrid] " fmt "\r\n", ##__VA_ARGS__)
#else
  #define FG_LOG(fmt, ...) do {} while (0)
#endif

/*
  Zero-argument begin() support.

  #define these BEFORE #include <Fluxgrid.h> (top of your sketch) and you can
  call Fluxgrid.begin() with no arguments — server, port and TLS are built in:

      #define WIFI_SSID  "your-wifi"
      #define WIFI_PASS  "your-password"
      #define FG_TOKEN   "paste-device-token"   // single string from the dashboard
      #include <Fluxgrid.h>

  FG_TOKEN is one opaque string per device — "<token>.<user>.<pass>" — that the
  library splits into a routing token + MQTT credentials.

  The empty fallbacks below let the header compile even when you use an
  explicit begin(...) overload instead. (Arduino compiles your .ino and the
  library separately, so the zero-arg begin() must live inline in this header
  to see your #defines — hence the "define before include" rule.)
*/
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define FG_FALLBACK_WIFI_SSID
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#define FG_FALLBACK_WIFI_PASS
#endif
#ifndef FG_TOKEN
#define FG_TOKEN ""
#define FG_FALLBACK_FG_TOKEN
#endif

// LED type constants for addLed()
#define LED_NORMAL  0
#define LED_WS2812  1

typedef void (*FluxgridEventFn)();

/*
  A value received from the cloud. read() and the onReceive() callback hand you
  one of these instead of a bare float, so the same call site can pull out an
  int, a float, a bool or the raw text — you choose how to interpret it:

      int    r = Fluxgrid.read("red").asInt();
      float  t = Fluxgrid.read("temp").asFloat();
      bool   on= Fluxgrid.read("pump").asBool();
      String s = Fluxgrid.read("note").asString();

  This mirrors how Blynk's `param` and Adafruit IO's `data` objects work, and
  sidesteps C++'s inability to overload a function on its return type alone.
*/
class FluxValue {
public:
  FluxValue() : _raw(""), _num(0.0f), _has(false) {}
  explicit FluxValue(const String &raw) : _raw(raw), _num(raw.toFloat()), _has(true) {}

  int     asInt()    const { return (int)lroundf(_num); }   // rounds, not truncates
  long    asLong()   const { return (long)llroundf(_num); }
  float   asFloat()  const { return _num; }
  double  asDouble() const { return (double)_num; }
  bool    asBool()   const;                                  // "1"/"true"/"on" or >0.5
  String  asString() const { return _raw; }                 // original text
  bool    isEmpty()  const { return !_has || _raw.length() == 0; } // nothing received yet

private:
  String _raw;
  float  _num;
  bool   _has;
};

class FluxgridClass : public Print {
public:
  FluxgridClass();

  /*
    Zero-argument setup: reads WIFI_SSID / WIFI_PASS / FG_TOKEN, which you
    #define at the TOP of your sketch (before #include <Fluxgrid.h>). FG_TOKEN
    is the single per-device string from the dashboard. Server, port and TLS
    are built in — nothing else to configure. Defined inline below the class so
    it can see your #defines.
  */
  void begin();

  /* Explicit form: pass the split per-device credentials instead of #defining
     them. token routes, mqttUser/mqttPass authenticate. */
  void begin(const char *ssid, const char *pass, const char *token, const char *mqttUser, const char *mqttPass);
  /* Use this overload if you set the device with setDevice() first. */
  void begin(const char *ssid, const char *pass);

  /* ── WiFi & connectivity control (all optional) ───────────────────────────
     By default the library OWNS WiFi: zero-arg begin() joins WIFI_SSID /
     WIFI_PASS in STA mode and reconnects for you. The calls below let you take
     it over or run without the cloud. */

  /* Hand WiFi to your own code — call BEFORE begin(). With manageWiFi(false)
     the library never touches the radio: bring up STA / AP / AP+STA yourself
     (or via WiFiManager, Improv, …) and Fluxgrid rides on top. run() then only
     observes the link, it never drives it (no mode changes, no reconnect kicks,
     never blocks). */
  void manageWiFi(bool on);

  /* Cloud (MQTT) on/off — call BEFORE begin(). Default on. cloud(false) makes a
     pure local device with no broker (e.g. an offline AP dashboard). */
  void cloud(bool on);

  /* Bring up / tear down the device's own WiFi access point. Convenience for
     local access without writing WiFi.softAP() yourself; uses AP+STA so an
     existing STA link (and the cloud) keeps working alongside the AP. Safe to
     call after begin(); pass no password for an open AP. */
  void startAP(const char *ssid, const char *pass = nullptr);
  void stopAP();

  /* ── Local bridge hooks ───────────────────────────────────────────────────
     Used by the optional, header-only FluxgridLocal companion (offline / AP
     dashboard) to tee data between this client and a local web server. Harmless
     if you never use FluxgridLocal. */

  /* Register a tee fired on every write(handle, value) — including when the
     cloud is off or disconnected — so a local server can push telemetry over
     SSE. */
  void onWrite(std::function<void(const char *handle, const String &value)> fn);

  /* Apply a value as if it arrived from the cloud: updates read()'s cache and
     fires the matching onReceive() callback. A local /write endpoint routes
     control writes through here so they behave identically to cloud writes. */
  void inject(const char *pin, const String &value);

  /* Optional — call BEFORE begin(): */
  /* Single combined device token "<token>.<user>.<pass>" — the library splits
     it into a routing token + MQTT user/pass. This is what the dashboard hands
     you, and what zero-arg begin() uses. */
  void setDevice(const char *combinedToken);
  /* Per-device MQTT credentials (recommended): token for routing, user/pass for auth. */
  void setDevice(const char *token, const char *mqttUser, const char *mqttPass);
  void setServer(const char *host, uint16_t port);

  /* Pick the Fluxgrid cloud region — call BEFORE begin().

     Fluxgrid runs one cloud per region, each with its own broker, accounts and
     devices. A device belongs to exactly ONE of them: the region you signed up
     in. Its credentials do not exist on the other, so pointing a device at the
     wrong region fails to authenticate.

         "com"  mqtt.lonelybinary.com   (default — global)
         "cn"   mqtt.lonelybinary.cn    (China)

     You only need this if you are on the China cloud:

         Fluxgrid.region("cn");
         Fluxgrid.begin();

     The sketch the dashboard generates for you already has the right line, so
     copy from there and you can ignore this. An unknown code is ignored (the
     default stands) and logged when debug is on. setServer() overrides it. */
  void region(const char *code);

  void secure(bool on = true);

  /*
    Pin the broker's TLS certificate — call BEFORE begin().

    By default TLS encrypts the link but accepts ANY server certificate
    (the connection cannot be eavesdropped, but an active attacker who can
    redirect your traffic could impersonate the broker). Pass your broker's
    root CA certificate in PEM form and the device will refuse to connect
    to anything else:

        static const char ROOT_CA[] = R"PEM(
        -----BEGIN CERTIFICATE-----
        ...your broker's root CA...
        -----END CERTIFICATE-----
        )PEM";

        Fluxgrid.setCACert(ROOT_CA);
        Fluxgrid.begin();

    The string must stay alive for the lifetime of the connection (a global
    or static literal — as above — is the usual way). Only used when TLS is
    on (the default); ignored after secure(false).
  */
  void setCACert(const char *pem);

  /*
    Enable over-the-air (OTA) firmware updates via the Arduino IDE.
    ⚠ IMPORTANT — boundary: this is LAN OTA only. Your computer and the
    ESP32 must be on the same local network. "Tools ▸ Port" will show a
    network port after the first USB flash. Remote / cloud OTA is a
    separate feature (cloud-compile + MinIO push).
  */
  void enableOTA(const char *password, const char *hostname = nullptr);

  /* Call this on every loop(). Keeps the connection alive + handles writes. */
  void run();
  bool connected();

  /* ── read / write API ─────────────────────────────────────────────────── */
  /* Push a value to a datastream. Overloaded by type — pass an int, float,
     double, bool, C-string or String and the right one is selected for you. */
  void write(const char *handle, int value);
  void write(const char *handle, long value);
  // Unsigned overloads matter more than they look: ESP.getFreeHeap() and
  // friends return uint32_t, which converts equally well to int/long/float/
  // double/bool — so without these the most natural call on an ESP32,
  //   Fluxgrid.write("heap", ESP.getFreeHeap());
  // is ambiguous and fails to compile.
  void write(const char *handle, unsigned int value);
  void write(const char *handle, unsigned long value);
  void write(const char *handle, float value);
  void write(const char *handle, double value);
  void write(const char *handle, bool value);
  void write(const char *handle, const char *value);
  void write(const char *handle, const String &value);

  /* Print interface — Fluxgrid.println(...) / .print(...) / .printf(...) mirror
     a line to BOTH the USB Serial Monitor and the "log" datastream (so a
     Terminal widget shows it), leaving the real Serial untouched. This is the
     explicit alternative to the FLUXGRID_CAPTURE_SERIAL Serial-redirect macro;
     both share one line buffer and the same log handle. */
  size_t write(uint8_t b) override;            // one byte into the log line buffer
  /* Change the datastream handle the captured log lines are published to
     (default "log"). Call once in setup(). */
  void   setLogHandle(const char *handle);

  /* Read the last value received on a datastream. The returned FluxValue
     converts on demand: .asInt() / .asFloat() / .asBool() / .asString().
     Returns an empty FluxValue (isEmpty() == true) if nothing has arrived. */
  FluxValue read(const char *handle);
  bool      has(const char *handle);          // true once a value has arrived

  /* React to incoming writes. The lambda receives a FluxValue:
       Fluxgrid.onReceive("pump", [](FluxValue v){ ... v.asBool() ... }); */
  void onReceive(const char *handle, std::function<void(FluxValue)> fn);

  /*
    WiFi scan → WiFi Scan widget.

    Scan for nearby access points and publish them to a datastream a WiFi Scan
    widget reads. The payload is a JSON array, strongest signal first, capped to
    `maxNets` and trimmed to fit the MQTT buffer:

      [{"ssid":"NET","rssi":-62,"channel":4,"enc":"WPA2","bssid":"AA:BB:.."},…]

    Blocking form — runs the scan (1–4 s, blocks loop() so MQTT keepalive pauses
    for that window) then publishes. Returns the number of networks published,
    or a negative value if the scan failed. Call it on a timer, not every loop:

        if (millis() - last > 10000) { last = millis(); Fluxgrid.writeWiFiScan(); }
  */
  int writeWiFiScan(const char *handle = "wifi_scan", int maxNets = 16);

  /*
    Non-blocking form — keeps Fluxgrid.run()/MQTT responsive during the scan.
    Call startWiFiScan() once to kick the radio, then call pollWiFiScan() every
    loop(): it returns -1 while the scan is still running, -2 if none is in
    flight, or the published count once results are ready (and publishes them).

        Fluxgrid.startWiFiScan();
        ...
        int c = Fluxgrid.pollWiFiScan();
        if (c >= 0) { ...done — kick the next one when you're ready... }
  */
  void startWiFiScan();
  int  pollWiFiScan(const char *handle = "wifi_scan", int maxNets = 16);

  /*
    One-line automatic memory reporting → Memory Chart widget.

    Call once in setup() and the library publishes the device's heap (and PSRAM,
    when fitted) to the cloud every `intervalMs` on its own — no per-loop write()
    calls, no datastreams to create or bind:

        void setup() {
          Fluxgrid.reportMemory();     // heap + PSRAM, every 5 s
          Fluxgrid.begin();
        }

    All the numbers ride in ONE MQTT message on a dedicated `mem` topic (not five
    separate telemetry writes), and the dashboard's Memory Chart widget just picks
    this device — exactly like the Device Info widget. Totals (heap / PSRAM size)
    already come from the device info the library reports on connect.

    What's published each tick (heap always; PSRAM only when ESP.getPsramSize()>0):
      free_heap      ESP.getFreeHeap()      heap bytes free right now
      min_free_heap  ESP.getMinFreeHeap()   heap low-water mark since boot
      max_alloc      ESP.getMaxAllocHeap()  largest single block still allocatable
      free_psram     ESP.getFreePsram()     PSRAM bytes free right now
      min_free_psram ESP.getMinFreePsram()  PSRAM low-water mark since boot

    Memory moves slowly, so 5 s keeps the chart live without spamming the broker;
    pass a different interval for a snappier demo (2000) or long-term leak watch
    (30000+). Pass 0 to turn it back off.
  */
  void reportMemory(uint32_t intervalMs = 5000);

  /* Called once each time the cloud connection is (re)established. */
  void onConnected(FluxgridEventFn fn);

  /*
    Generic firmware mode: subscribe to config topic, parse manifest,
    auto-wire ADC inputs / digital outputs / PWM. Hot-update on new config.
    Call BEFORE begin(). See config schema in README.
  */
  void autoConfig();

  /*
    LED control — works with both regular LEDs and WS2812.
    Call addLed() once in setup() to register the GPIO and its type, then
    use ledOn() / ledOff() anywhere.

        Fluxgrid.addLed(26);                         // regular LED
        Fluxgrid.addLed(48, LED_WS2812);             // WS2812 (default white)
        Fluxgrid.addLed(48, LED_WS2812, 255, 80, 0); // WS2812, custom default colour

        Fluxgrid.ledOn(26);                          // regular → HIGH
        Fluxgrid.ledOn(48);                          // WS2812 → stored colour
        Fluxgrid.ledOn(48, 0, 200, 255);             // WS2812 → new colour (stored)
        Fluxgrid.ledOff(48);                         // WS2812 → (0,0,0)
  */
  void addLed(uint8_t gpio, uint8_t type = LED_NORMAL,
              uint8_t r = 255, uint8_t g = 255, uint8_t b = 255);
  void ledOn(uint8_t gpio);
  void ledOn(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b);
  void ledOff(uint8_t gpio);

  /*
    ── File explorer ─────────────────────────────────────────────────────────
    Expose a mounted filesystem to the dashboard's File Explorer widget: browse,
    download, upload, rename, delete and format — all from the browser.

    YOU mount the hardware (the library never touches the SD bus / pins): bring
    up LittleFS / FFat / SD / SD_MMC in setup(), register each mounted fs::FS as
    a named volume with addVolume(), then call enableFiles() once. Example:

        #include <LittleFS.h>
        #include <SD_MMC.h>
        void setup() {
          LittleFS.begin(true);
          Fluxgrid.addVolume("flash", LittleFS, FG_FLASH);  // capacity auto-detected
          SD_MMC.begin();
          Fluxgrid.addVolume("sd", SD_MMC, FG_SD);
          Fluxgrid.enableFiles();
          Fluxgrid.begin();
        }
        void loop() { Fluxgrid.run(); }

    `id` is a short slug you choose ([A-Za-z0-9_-], ≤32 chars) and is how the
    widget addresses the volume; `label` is the human name shown on the card
    (defaults to `id`). Mark a volume `readonly` to refuse writes from the cloud.

    Two registration styles:

    • Known types (below) — pass the global object (LittleFS / SD / SD_MMC /
      FFat) and its FG_* type; capacity is read for you. This is the template
      overload: it is only instantiated when you call it, so the concrete FS
      class (and its static totalBytes()/usedBytes(), which the fs::FS base does
      NOT expose) is in scope from your sketch's own #include.

    • Any FS (the escape hatch) — the explicit overload further down takes two
      capacity getter lambdas, so you can register a filesystem the known-type
      path doesn't recognise.
  */
  template <typename T>
  void addVolume(const char *id, T &fs, uint8_t type,
                 const char *label = nullptr, bool readonly = false) {
    // Capture the concrete object's ADDRESS so totalBytes()/usedBytes() — and,
    // for flash, format() — resolve against T here in the sketch's translation
    // unit, not the fs::FS base (which lacks them). The lambdas are stored and
    // called later from the .cpp, where only fs::FS is visible. These globals
    // (LittleFS / SD / SD_MMC / FFat) have program lifetime, so the captured
    // pointer never dangles.
    T *p = &fs;
    std::function<uint64_t()> totalFn = [p]() -> uint64_t { return p->totalBytes(); };
    std::function<uint64_t()> usedFn  = [p]() -> uint64_t { return p->usedBytes(); };
    // A real format() only exists for on-chip flash types (LittleFS / FFat); SD
    // classes have none. _fgMakeFormatFn() compiles to the real call only when
    // T actually has format() (SFINAE) — so this instantiates cleanly for an SD
    // type too — and we only USE it for FG_FLASH volumes.
    std::function<bool()> formatFn =
        (type == FG_FLASH) ? _fgMakeFormatFn(p, 0) : std::function<bool()>();
    _addVolume(id, &fs, type, label, readonly, totalFn, usedFn, formatFn);
  }

  /*
    Escape hatch — register ANY fs::FS by supplying the capacity getters
    yourself (the fs::FS base class can't report them). `totalFn`/`usedFn`
    return bytes; pass nullptr for either if unknown (reported as 0). No
    format() is wired for this path — a "format" request wipes the volume by
    deleting everything at root instead.

        Fluxgrid.addVolume("ram", myFs, FG_FS_OTHER,
                           [](){ return myFs.totalBytes(); },
                           [](){ return myFs.usedBytes();  });
  */
  void addVolume(const char *id, fs::FS &fs, uint8_t type,
                 std::function<uint64_t()> totalFn,
                 std::function<uint64_t()> usedFn,
                 const char *label = nullptr, bool readonly = false);

  /*
    Turn the file-explorer handler on. Call once in setup() (before begin() is
    fine). Only then does the device subscribe to its fs/req command topic, so
    sketches that don't use files never pay for it.
  */
  void enableFiles();

  /* internal — invoked by the MQTT callback trampoline */
  void _dispatch(char *topic, uint8_t *payload, unsigned int len);

private:
  void connectWiFi();
  /* Shared by _dispatch (cloud) and inject (local): update read() cache + fire onReceive. */
  void _applyIncoming(const char *pin, const String &raw);
  std::function<void(const char *, const String &)> _onWrite = nullptr;
  bool connectCloudOnce();
  // Build the retained `meta` payload sent on each cloud connect: the joined
  // SSID plus an `info` block of chip / memory / MAC / firmware / network facts
  // for the Device Info widget. Serialized with ArduinoJson (escapes for us).
  String buildMetaPayload();
  // Build the periodic `mem` payload for reportMemory(): one JSON object with the
  // live heap (and PSRAM, when fitted) numbers the Memory Chart widget reads.
  String buildMemPayload();
  String base() const;

  const char *_ssid    = nullptr;
  const char *_pass    = nullptr;
  const char *_token   = "";
  const char *_key     = "";       // per-device MQTT password
  const char *_mqttUser = nullptr; // per-device MQTT username
  // Backing storage when the credentials come from a single combined token
  // (setDevice("<token>.<user>.<pass>")); _token/_mqttUser/_key point in here.
  char _tokenBuf[48] = {0};
  char _userBuf[24]  = {0};
  char _passBuf[40]  = {0};
  const char *_host    = "mqtt.lonelybinary.com";
  uint16_t    _port    = 8883;   // platform default: MQTTS (TLS)
  bool        _secure  = true;   // encrypted by default; secure(false) opts out
  const char *_caCert  = nullptr; // root CA PEM; null = encrypt but don't verify

  WiFiClient       _net;
  WiFiClientSecure _netSecure;
  PubSubClient     _mqtt;
  unsigned long    _lastTry = 0;
  unsigned long    _lastBeat = 0;   // last "online" heartbeat republish (ms)
  unsigned long    _lastWifiTry = 0;// last non-blocking WiFi reconnect kick (ms)
  bool             _started = false;
  bool             _manageWifi = true;  // false → the sketch owns the radio
  bool             _cloud      = true;  // false → no MQTT (local-only device)
  bool             _apOn       = false; // soft-AP currently up
  FluxgridEventFn  _onConnected = nullptr;
  bool             _otaEnabled  = false;

  // ── Automatic memory reporting (reportMemory) ──────────────────────────────
  bool          _reportMem   = false; // publish `mem` snapshots on a timer
  uint32_t      _memInterval = 5000;  // ms between snapshots
  unsigned long _lastMem     = 0;     // last snapshot publish (millis)

  // Per-datastream slot: cache + optional callback
  struct Slot {
    char pin[24];
    float value;
    bool  has;
    String raw;
    std::function<void(FluxValue)> fn; // null if none
  };
  Slot    _slots[FLUXGRID_MAX_SLOTS];
  uint8_t _scount = 0;

  Slot *findSlot(const char *pin);
  Slot *findOrCreateSlot(const char *pin);

  // ── WiFi scan internals ────────────────────────────────────────────────
  // Serialize the completed scan (n results) and publish to `handle`.
  int _publishWiFiScan(const char *handle, int n, int maxNets);
  static const char *_wifiEncName(wifi_auth_mode_t mode);

  // ── autoConfig internals ──────────────────────────────────────────────
  bool _acEnabled = false;

  struct AcInput {
    char    pin[24];
    uint8_t gpio;
    char    source[12]; // "adc" | "digital-in" | "sensor"
    uint32_t interval;  // ms
    float   mapIn[2];   // [inMin, inMax]
    float   mapOut[2];  // [outMin, outMax]
    unsigned long lastSent;
  };

  // ── Sensor driver support ─────────────────────────────────────────────
  // source == "sensor": driver = "dht22"|"dht11"|"bme280"|"ds18b20"
  // Multiple virtual pins per physical sensor (temp, hum, press, etc.)
  struct AcSensor {
    char     driver[12];   // "dht22"|"dht11"|"bme280"|"ds18b20"
    char     pinTemp[24];  // datastream handle for temperature (or generic)
    char     pinHum[24];   // datastream handle for humidity (dht/bme)
    char     pinPress[24]; // datastream handle for pressure (bme280)
    uint8_t  gpio;         // data pin / 1-wire bus GPIO
    uint8_t  addr;         // I2C address (bme280, default 0x76)
    uint32_t interval;     // ms
    unsigned long lastSent;
    void    *obj;          // runtime sensor object (heap, cast in .cpp)
  };
  struct AcOutput {
    char    pin[24];
    uint8_t gpio;
    char    target[12]; // "digital" | "analog"
  };
  struct AcPwm {
    char     pin[24];
    uint8_t  gpio;
    uint32_t freq;
    uint8_t  resolution;
    uint8_t  channel;
  };

  AcInput  _acInputs[FLUXGRID_MAX_INPUTS];
  AcOutput _acOutputs[FLUXGRID_MAX_OUTPUTS];
  AcPwm    _acPwms[FLUXGRID_MAX_PWM];
  AcSensor _acSensors[4];
  uint8_t  _acInputCount  = 0;
  uint8_t  _acOutputCount = 0;
  uint8_t  _acPwmCount    = 0;
  uint8_t  _acSensorCount = 0;

  void applyConfig(const String &json);
  void runAutoConfig();
  static float linearMap(float v, float i0, float i1, float o0, float o1);

  // ── LED control internals ─────────────────────────────────────────────────
  struct LedEntry {
    uint8_t gpio;
    uint8_t type;   // LED_NORMAL or LED_WS2812
    uint8_t r, g, b;
    uint8_t rmtIdx; // WS2812 only: index among WS2812 entries (for RMT channel, 2.x)
  };
  LedEntry _leds[8];
  uint8_t  _ledCount    = 0;
  uint8_t  _ws2812Count = 0; // how many WS2812 entries registered so far

  LedEntry *_findLed(uint8_t gpio);
  void      _ws2812Send(uint8_t gpio, uint8_t rmtIdx, uint8_t r, uint8_t g, uint8_t b);

  // ── File explorer internals ───────────────────────────────────────────────
  // One registered volume. `fs` is the mounted filesystem; the *Fn lambdas were
  // bound to the concrete object in the templated addVolume so they can read
  // capacity / format it without the base class exposing those calls.
  struct Volume {
    char     id[24];
    char     label[32];
    uint8_t  type;        // FG_FLASH | FG_SD | FG_FS_OTHER
    bool     readonly;
    fs::FS  *fs;
    std::function<uint64_t()> totalFn; // bytes, or null → 0
    std::function<uint64_t()> usedFn;  // bytes, or null → 0
    std::function<bool()>     formatFn; // null → wipe-all on "format"
  };
  Volume  _volumes[FLUXGRID_MAX_VOLUMES];
  uint8_t _volCount     = 0;
  bool    _filesEnabled = false;

  // Shared, non-template registration the addVolume overloads forward into.
  void _addVolume(const char *id, fs::FS *fs, uint8_t type, const char *label,
                  bool readonly, std::function<uint64_t()> totalFn,
                  std::function<uint64_t()> usedFn, std::function<bool()> formatFn);
  Volume     *_findVolume(const char *id);
  static const char *_volTypeName(uint8_t type);

  // Metadata ops — run synchronously inside _dispatch (main loop) and publish
  // their own reply. Each takes the parsed request doc and the echo id.
  void _fsDispatch(const char *json, unsigned int len);
  void _fsVolumes(const char *id);
  void _fsList(const char *id, Volume *v, const char *path, int limit, const char *cursor);
  void _fsStat(const char *id, Volume *v, const char *path);
  void _fsMkdir(const char *id, Volume *v, const char *path);
  void _fsDelete(const char *id, Volume *v, const char *path);
  void _fsRename(const char *id, Volume *v, const char *path, const char *to);
  void _fsFormat(const char *id, Volume *v);
  // Reply helpers (publish to fs/res from the main thread).
  void _fsReplyOk(const char *id);
  void _fsReplyErr(const char *id, const char *err);
  void _fsPublish(const String &payload);
  static bool _fsPathOk(const char *p);
  // Recursively delete everything under `path` on v->fs (path itself too unless
  // keepRoot). Used by delete (dir) and SD "format" (root, keepRoot=true).
  static bool _fsRmRf(fs::FS *fs, const String &path, bool keepRoot);

  // ── Bulk transfer (pull/push) — runs on its own core-0 task ────────────────
  // The task does ONLY HTTP + file IO and never touches MQTT. It writes status
  // here under _xferMux; run() polls it and publishes progress/result from the
  // main thread (PubSubClient is single-threaded).
  enum XferKind : uint8_t { XFER_NONE = 0, XFER_PULL, XFER_PUSH };
  struct Transfer {
    volatile bool      active   = false; // a task is running
    volatile bool      done     = false; // task finished — main thread must publish final + clean up
    volatile bool      ok       = false; // final outcome
    volatile bool      progress = false; // a new progress sample is waiting to be published
    volatile uint64_t  doneBytes = 0;
    volatile uint64_t  totalBytes = 0;
    char        id[40];
    char        err[12];
    XferKind    kind = XFER_NONE;
    Volume     *vol  = nullptr;
    String      path;
    String      url;
    TaskHandle_t task = nullptr;
  };
  Transfer        _xfer;
  portMUX_TYPE    _xferMux = portMUX_INITIALIZER_UNLOCKED;

  // Start a transfer (validates + spawns the task). Replies EBUSY itself if one
  // is already running. id/path/url copied in before the task starts.
  void _fsStartTransfer(XferKind kind, const char *id, Volume *v,
                        const char *path, const char *url, uint64_t size);
  void _fsPollTransfer();         // called every run(): publish progress/result
  static void _xferTaskTrampoline(void *arg);
  void _xferRun();                // the task body (HTTP + file IO)
  // Configure a secure client for the presigned URL (pin CA if we have one).
  void _fsPrepSecure(WiFiClientSecure &c);
};

/* The one global instance you use everywhere. */
extern FluxgridClass Fluxgrid;

/*
  Zero-arg begin() — inline so it expands in your sketch's translation unit and
  picks up the WIFI_SSID / WIFI_PASS / FG_TOKEN macros you #defined above the
  include. setDevice() splits the single FG_TOKEN into token + user + pass.
*/
inline void FluxgridClass::begin() {
  setDevice(FG_TOKEN);
  begin(WIFI_SSID, WIFI_PASS);
}

/*
  Drop the placeholder macros now that the only code that reads them (the
  inline begin() above) has been preprocessed.

  Leaving them defined rewrites the NAME anywhere later in your sketch, which
  is invisible and confusing when it bites. The ByoWiFi example manages WiFi
  itself and quite reasonably declares

      const char *WIFI_SSID = "your-wifi";

  after the include — which became `const char *"" = "your-wifi";` and failed
  to compile with an error pointing at this header, not at the sketch.

  Only the placeholders THIS header invented are removed. A real #define from
  your sketch is left alone, because your code may legitimately use it again
  (as several examples do when they call WiFi.begin() themselves).
*/
#ifdef FG_FALLBACK_WIFI_SSID
#undef WIFI_SSID
#undef FG_FALLBACK_WIFI_SSID
#endif
#ifdef FG_FALLBACK_WIFI_PASS
#undef WIFI_PASS
#undef FG_FALLBACK_WIFI_PASS
#endif
#ifdef FG_FALLBACK_FG_TOKEN
#undef FG_TOKEN
#undef FG_FALLBACK_FG_TOKEN
#endif

/*
  The object that FLUXGRID_CAPTURE_SERIAL aliases Serial to. A Print that tees:
  every byte goes to the real Serial (so the USB monitor is unaffected) and is
  also accumulated into a line buffer; on '\n' the completed line is published
  to the log datastream. Input + status calls (read/available/if(Serial)) are
  delegated to the real Serial so redirected sketches keep working unchanged.

  NOTE: this is defined BEFORE the `#define Serial` below, so every `Serial`
  here still means the real Arduino HardwareSerial.
*/
class FluxgridSerial : public Print {
public:
  void begin(unsigned long baud)                 { Serial.begin(baud); }
  // Templated so the two-arg body is only instantiated if the sketch actually
  // calls it: when Serial is USB CDC (HWCDC), begin() takes baud only, so a
  // non-template overload would fail to compile even when unused.
  template <typename Config>
  void begin(unsigned long baud, Config config)  { Serial.begin(baud, config); }
  void end()                                     { Serial.end(); }
  /* Datastream handle the captured lines are published to (default "log"). */
  void setLogHandle(const char *handle) { if (handle && handle[0]) _handle = handle; }

  size_t write(uint8_t b) override {
    Serial.write(b);                 // always keep the USB monitor working
    if (b == '\r') return 1;         // swallow CR; lines terminate on '\n'
    if (b == '\n') { flushLine(); return 1; }
    if (_n < sizeof(_line) - 1) _line[_n++] = (char)b; // overflow truncates the line
    return 1;
  }
  size_t write(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buf[i]);
    return size;
  }

  // Delegate the rest to the real Serial so a redirected `Serial.read()`,
  // `Serial.available()` or `if (Serial)` still hit the hardware UART.
  int  available() { return Serial.available(); }
  int  read()      { return Serial.read(); }
  int  peek()      { return Serial.peek(); }
  void flush()     { Serial.flush(); }
  operator bool()  { return (bool)Serial; }

  const char *logHandle() const { return _handle; }

private:
  void flushLine() {
    _line[_n] = '\0';
    Fluxgrid.write(_handle, _line); // publishes; no-op (USB only) while offline
    _n = 0;
  }
  const char *_handle = "log";
  char        _line[200];           // one serial line; longer lines truncate
  size_t      _n = 0;
};

/* The global tee. Fluxgrid.println(...) routes through this same instance so
   both entry points share one line buffer and one log handle. */
extern FluxgridSerial FluxgridSerialPort;

/*
  Zero-code-change capture: replace the Serial token with the tee. This affects
  every `Serial` AFTER this include in the same .ino — Serial1/Serial2 (distinct
  tokens) are untouched, and the library's own .cpp is compiled separately so
  its internal Serial stays the real one (no recursion).
*/
#if FLUXGRID_CAPTURE_SERIAL
  #define Serial FluxgridSerialPort
#endif

#endif // FLUXGRID_H
