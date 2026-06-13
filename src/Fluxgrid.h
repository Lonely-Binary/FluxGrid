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

  Requires: ESP32 Arduino core + PubSubClient (knolleary).
  MIT License · Lonely Binary
*/
#ifndef FLUXGRID_H
#define FLUXGRID_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <functional>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

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
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef FG_TOKEN
#define FG_TOKEN ""
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

  /* Optional — call BEFORE begin(): */
  /* Single combined device token "<token>.<user>.<pass>" — the library splits
     it into a routing token + MQTT user/pass. This is what the dashboard hands
     you, and what zero-arg begin() uses. */
  void setDevice(const char *combinedToken);
  /* Per-device MQTT credentials (recommended): token for routing, user/pass for auth. */
  void setDevice(const char *token, const char *mqttUser, const char *mqttPass);
  void setServer(const char *host, uint16_t port);
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

  /* internal — invoked by the MQTT callback trampoline */
  void _dispatch(char *topic, uint8_t *payload, unsigned int len);

private:
  void connectWiFi();
  bool connectCloudOnce();
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
  const char *_host    = "mqtt.lonelybinary.cn";
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
  FluxgridEventFn  _onConnected = nullptr;
  bool             _otaEnabled  = false;

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
