#include "Fluxgrid.h"

// The library body must always talk to the real UART. If FLUXGRID_CAPTURE_SERIAL
// is set as a global build flag (not just at the top of the sketch), the header
// aliases `Serial` to the FluxgridSerial tee here too — which would route the
// internal [Fluxgrid] debug log back through the tee and pollute the cloud "log"
// stream. Undo *that* alias for this translation unit and point Serial back at
// the real core device. NOTE: the esp32 core ALWAYS defines `Serial` as a macro
// (Serial0 / HWCDCSerial / USBSerial), so an unconditional `#ifdef Serial /
// #undef Serial` would strip Serial from the whole file even when capture is
// off — hence the FLUXGRID_CAPTURE_SERIAL guard and the explicit restore, which
// mirrors the selection in HardwareSerial.h.
#if FLUXGRID_CAPTURE_SERIAL
  #undef Serial
  #if ARDUINO_USB_CDC_ON_BOOT
    #if ARDUINO_USB_MODE
      #define Serial HWCDCSerial
    #else
      #define Serial USBSerial
    #endif
  #else
    #define Serial Serial0
  #endif
#endif

// ── WS2812 RMT driver ─────────────────────────────────────────────────────────
// Arduino core 3.x exposes a simplified RMT wrapper; 2.x uses the raw ESP-IDF
// driver. We detect the version exactly as the LEDC PWM code above does.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #include <driver/rmt_tx.h>
#else
  #include <driver/rmt.h>
#endif

// ── Optional sensor library support (auto-detected at compile time) ────────────
#if __has_include(<DHT.h>)
  #include <DHT.h>
  #define FG_HAS_DHT 1
#endif
#if __has_include(<Adafruit_BME280.h>)
  #include <Adafruit_BME280.h>
  #include <Wire.h>
  #define FG_HAS_BME280 1
#endif
#if __has_include(<DallasTemperature.h>)
  #include <DallasTemperature.h>
  #include <OneWire.h>
  #define FG_HAS_DS18B20 1
// File-scope so all three sites share one type: applyConfig allocates it, the
// reconfig cleanup loop deletes it, and runAutoConfig reads it. It used to be a
// struct defined locally inside applyConfig/runAutoConfig, so the cleanup loop
// couldn't name the type and never freed it — every pushed config leaked one
// bundle (OneWire + DallasTemperature) on the heap.
struct FgDs18b20 {
  OneWire ow;
  DallasTemperature dt;
  FgDs18b20(uint8_t p) : ow(p), dt(&ow) {}
};
#endif

FluxgridClass Fluxgrid;
FluxgridSerial FluxgridSerialPort;

static void _fg_trampoline(char *topic, uint8_t *payload, unsigned int len) {
  Fluxgrid._dispatch(topic, payload, len);
}

FluxgridClass::FluxgridClass() {
  for (uint8_t i = 0; i < FLUXGRID_MAX_SLOTS; i++) {
    _slots[i].pin[0] = '\0';
    _slots[i].value  = 0.0f;
    _slots[i].has    = false;
  }
}

// ── Configuration ─────────────────────────────────────────────────────────────
void FluxgridClass::setDevice(const char *combinedToken) {
  // Split a single "<token>.<user>.<pass>" string into its three parts. The
  // dashboard joins them with '.', which never appears in the values. If the
  // delimiters are missing (e.g. a bare legacy token), use the whole string as
  // the routing token and leave user/pass empty.
  if (!combinedToken) { _token = ""; return; }
  const char *d1 = strchr(combinedToken, '.');
  const char *d2 = d1 ? strchr(d1 + 1, '.') : nullptr;
  if (!d1 || !d2) { _token = combinedToken; return; }

  size_t nT = (size_t)(d1 - combinedToken);
  size_t nU = (size_t)(d2 - (d1 + 1));
  size_t nP = strlen(d2 + 1);
  if (nT >= sizeof(_tokenBuf) || nU >= sizeof(_userBuf) || nP >= sizeof(_passBuf)) {
    _token = combinedToken; return; // unexpectedly long — fail safe
  }
  memcpy(_tokenBuf, combinedToken, nT); _tokenBuf[nT] = '\0';
  memcpy(_userBuf,  d1 + 1,        nU); _userBuf[nU]  = '\0';
  memcpy(_passBuf,  d2 + 1,        nP); _passBuf[nP]  = '\0';
  _token    = _tokenBuf;
  _mqttUser = _userBuf;
  _key      = _passBuf;
}
void FluxgridClass::setDevice(const char *token, const char *mqttUser, const char *mqttPass) {
  _token = token; _mqttUser = mqttUser; _key = mqttPass;
}
void FluxgridClass::setServer(const char *host, uint16_t port)   { _host = host; _port = port; }
void FluxgridClass::secure(bool on) {
  _secure = on;
  if (on && _port == 1883) _port = 8883;
}
void FluxgridClass::setCACert(const char *pem) { _caCert = pem; }
void FluxgridClass::onConnected(FluxgridEventFn fn) { _onConnected = fn; }

// ── Slot helpers ──────────────────────────────────────────────────────────────
FluxgridClass::Slot *FluxgridClass::findSlot(const char *pin) {
  for (uint8_t i = 0; i < _scount; i++)
    if (strncmp(_slots[i].pin, pin, sizeof(_slots[i].pin)) == 0) return &_slots[i];
  return nullptr;
}

FluxgridClass::Slot *FluxgridClass::findOrCreateSlot(const char *pin) {
  Slot *s = findSlot(pin);
  if (s) return s;
  if (_scount >= FLUXGRID_MAX_SLOTS) return nullptr;
  s = &_slots[_scount++];
  strncpy(s->pin, pin, sizeof(s->pin) - 1);
  s->pin[sizeof(s->pin) - 1] = '\0';
  s->value = 0.0f;
  s->has   = false;
  s->fn    = nullptr;
  return s;
}

// ── FluxValue ─────────────────────────────────────────────────────────────────
bool FluxValue::asBool() const {
  String s = _raw;
  s.trim();
  s.toLowerCase();
  if (s == "true" || s == "on" || s == "yes") return true;
  if (s == "false" || s == "off" || s == "no" || s.length() == 0) return false;
  return _num > 0.5f;
}

// ── read API ──────────────────────────────────────────────────────────────────
FluxValue FluxgridClass::read(const char *handle) {
  Slot *s = findSlot(handle);
  return (s && s->has) ? FluxValue(s->raw) : FluxValue();
}

bool FluxgridClass::has(const char *handle) {
  Slot *s = findSlot(handle);
  return s ? s->has : false;
}

void FluxgridClass::onReceive(const char *handle, std::function<void(FluxValue)> fn) {
  Slot *s = findOrCreateSlot(handle);
  if (s) s->fn = fn;
}

// ── autoConfig ────────────────────────────────────────────────────────────────
void FluxgridClass::autoConfig() {
  _acEnabled = true;
}

float FluxgridClass::linearMap(float v, float i0, float i1, float o0, float o1) {
  if (i1 == i0) return o0;
  return (v - i0) / (i1 - i0) * (o1 - o0) + o0;
}

void FluxgridClass::applyConfig(const String &json) {
  // Reset previous state (sensor objects freed below)
  _acInputCount = _acOutputCount = _acPwmCount = 0;
  // Free previous sensor objects
  for (uint8_t i = 0; i < _acSensorCount; i++) {
    if (_acSensors[i].obj) {
      // Cast and delete based on driver type
#ifdef FG_HAS_DHT
      if (strncmp(_acSensors[i].driver, "dht", 3) == 0) delete static_cast<DHT *>(_acSensors[i].obj);
#endif
#ifdef FG_HAS_BME280
      if (strcmp(_acSensors[i].driver, "bme280") == 0) delete static_cast<Adafruit_BME280 *>(_acSensors[i].obj);
#endif
#ifdef FG_HAS_DS18B20
      if (strcmp(_acSensors[i].driver, "ds18b20") == 0) delete static_cast<FgDs18b20 *>(_acSensors[i].obj);
#endif
      _acSensors[i].obj = nullptr;
    }
  }
  _acSensorCount = 0;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;

  // Inputs — split sensor entries into their own list
  JsonArrayConst inputs = doc["inputs"].as<JsonArrayConst>();
  for (JsonObjectConst inp : inputs) {
    const char *src = inp["source"] | "adc";
    if (strcmp(src, "sensor") == 0) {
      // ── Sensor driver entry ───────────────────────────────────────────
      if (_acSensorCount >= 4) continue;
      AcSensor &s = _acSensors[_acSensorCount++];
      s.obj = nullptr;
      strncpy(s.driver,   inp["driver"]   | "dht22", sizeof(s.driver)   - 1);  s.driver[sizeof(s.driver)-1] = '\0';
      strncpy(s.pinTemp,  inp["pinTemp"]  | "",       sizeof(s.pinTemp)  - 1);  s.pinTemp[sizeof(s.pinTemp)-1] = '\0';
      strncpy(s.pinHum,   inp["pinHum"]   | "",       sizeof(s.pinHum)   - 1);  s.pinHum[sizeof(s.pinHum)-1] = '\0';
      strncpy(s.pinPress, inp["pinPress"] | "",       sizeof(s.pinPress) - 1);  s.pinPress[sizeof(s.pinPress)-1] = '\0';
      s.gpio     = inp["gpio"]     | 4;
      s.addr     = inp["addr"]     | 0x76;
      s.interval = inp["interval"] | 5000;
      s.lastSent = 0;
      // Instantiate and begin sensor
#ifdef FG_HAS_DHT
      if (strncmp(s.driver, "dht", 3) == 0) {
        uint8_t dhtType = (strcmp(s.driver, "dht11") == 0) ? DHT11 : DHT22;
        DHT *d = new DHT(s.gpio, dhtType);
        d->begin();
        s.obj = d;
      }
#endif
#ifdef FG_HAS_BME280
      if (strcmp(s.driver, "bme280") == 0) {
        Adafruit_BME280 *b = new Adafruit_BME280();
        b->begin(s.addr);
        s.obj = b;
      }
#endif
#ifdef FG_HAS_DS18B20
      if (strcmp(s.driver, "ds18b20") == 0) {
        // OneWire + DallasTemperature need to outlive this call, so heap-allocate
        // the shared FgDs18b20 bundle; the cleanup loop above frees it on reconfig.
        FgDs18b20 *bundle = new FgDs18b20(s.gpio);
        bundle->dt.begin();
        s.obj = bundle;
      }
#endif
      continue; // don't add to _acInputs
    }
    // ── ADC / digital-in entry ────────────────────────────────────────────
    if (_acInputCount >= FLUXGRID_MAX_INPUTS) continue;
    AcInput &a = _acInputs[_acInputCount++];
    strncpy(a.pin, inp["pin"] | "", sizeof(a.pin) - 1);
    a.pin[sizeof(a.pin) - 1] = '\0';
    strncpy(a.source, src, sizeof(a.source) - 1);
    a.source[sizeof(a.source) - 1] = '\0';
    a.gpio      = inp["gpio"]     | 0;
    a.interval  = inp["interval"] | 2000;
    a.lastSent  = 0;
    JsonArrayConst m = inp["map"].as<JsonArrayConst>();
    a.mapIn[0]  = m.size() >= 4 ? (float)m[0] : 0.0f;
    a.mapIn[1]  = m.size() >= 4 ? (float)m[1] : 4095.0f;
    a.mapOut[0] = m.size() >= 4 ? (float)m[2] : 0.0f;
    a.mapOut[1] = m.size() >= 4 ? (float)m[3] : 100.0f;
    if (strcmp(a.source, "digital-in") == 0) {
      pinMode(a.gpio, INPUT);
    }
    // ADC: analogRead works without explicit pinMode on ESP32
  }

  // Outputs
  JsonArrayConst outputs = doc["outputs"].as<JsonArrayConst>();
  for (JsonObjectConst out : outputs) {
    if (_acOutputCount >= FLUXGRID_MAX_OUTPUTS) break;
    AcOutput &o = _acOutputs[_acOutputCount++];
    strncpy(o.pin, out["pin"] | "", sizeof(o.pin) - 1);
    o.pin[sizeof(o.pin) - 1] = '\0';
    strncpy(o.target, out["target"] | "digital", sizeof(o.target) - 1);
    o.target[sizeof(o.target) - 1] = '\0';
    o.gpio = out["gpio"] | 0;
    pinMode(o.gpio, OUTPUT);
    // Register onReceive to drive this output when a write arrives
    // Capture by value to avoid dangling pointer
    uint8_t gpio = o.gpio;
    String  tgt  = String(o.target);
    onReceive(o.pin, [gpio, tgt](FluxValue v) {
      if (tgt == "digital") digitalWrite(gpio, v.asBool() ? HIGH : LOW);
      else                  analogWrite(gpio, v.asInt());
    });
  }

  // PWM channels
  JsonArrayConst pwms = doc["pwm"].as<JsonArrayConst>();
  for (JsonObjectConst pw : pwms) {
    if (_acPwmCount >= FLUXGRID_MAX_PWM) break;
    AcPwm &p = _acPwms[_acPwmCount];
    strncpy(p.pin, pw["pin"] | "", sizeof(p.pin) - 1);
    p.pin[sizeof(p.pin) - 1] = '\0';
    p.gpio       = pw["gpio"]  | 0;
    p.freq       = pw["freq"]  | 5000;
    p.resolution = pw["res"]   | 8;
    p.channel    = _acPwmCount; // one LEDC channel per PWM entry
    // LEDC API differs between ESP32 Arduino core 2.x and 3.x. In 3.x the
    // channel is allocated internally and you attach/write by GPIO pin.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(p.gpio, p.freq, p.resolution);
#else
    ledcSetup(p.channel, p.freq, p.resolution);
    ledcAttachPin(p.gpio, p.channel);
#endif
    _acPwmCount++;
    // Register onReceive to drive PWM duty
    uint8_t ch   = p.channel;
    uint8_t gpio = p.gpio;
    uint8_t res  = p.resolution;
    onReceive(p.pin, [ch, gpio, res](FluxValue v) {
      uint32_t maxDuty = (1u << res) - 1;
      uint32_t duty    = (uint32_t)(constrain(v.asFloat(), 0.0f, 100.0f) / 100.0f * maxDuty);
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
      ledcWrite(gpio, duty);   // 3.x: write by pin
#else
      (void)gpio;
      ledcWrite(ch, duty);     // 2.x: write by channel
#endif
    });
  }
}

void FluxgridClass::runAutoConfig() {
  unsigned long now = millis();
  // ── ADC / digital-in inputs ───────────────────────────────────────────────
  for (uint8_t i = 0; i < _acInputCount; i++) {
    AcInput &a = _acInputs[i];
    if (now - a.lastSent < a.interval) continue;
    a.lastSent = now;
    float raw;
    if (strcmp(a.source, "digital-in") == 0) {
      raw = digitalRead(a.gpio) ? 1.0f : 0.0f;
    } else {
      raw = (float)analogRead(a.gpio);
    }
    float mapped = linearMap(raw, a.mapIn[0], a.mapIn[1], a.mapOut[0], a.mapOut[1]);
    write(a.pin, mapped);
  }
  // ── Sensor drivers ────────────────────────────────────────────────────────
  for (uint8_t i = 0; i < _acSensorCount; i++) {
    AcSensor &s = _acSensors[i];
    if (!s.obj) continue;
    if (now - s.lastSent < s.interval) continue;
    s.lastSent = now;
#ifdef FG_HAS_DHT
    if (strncmp(s.driver, "dht", 3) == 0) {
      DHT *d = static_cast<DHT *>(s.obj);
      float t = d->readTemperature();
      float h = d->readHumidity();
      if (!isnan(t) && s.pinTemp[0]) write(s.pinTemp, t);
      if (!isnan(h) && s.pinHum[0])  write(s.pinHum,  h);
    }
#endif
#ifdef FG_HAS_BME280
    if (strcmp(s.driver, "bme280") == 0) {
      Adafruit_BME280 *b = static_cast<Adafruit_BME280 *>(s.obj);
      if (s.pinTemp[0])  write(s.pinTemp,  b->readTemperature());
      if (s.pinHum[0])   write(s.pinHum,   b->readHumidity());
      if (s.pinPress[0]) write(s.pinPress, b->readPressure() / 100.0f); // hPa
    }
#endif
#ifdef FG_HAS_DS18B20
    if (strcmp(s.driver, "ds18b20") == 0) {
      FgDs18b20 *bundle = static_cast<FgDs18b20 *>(s.obj);
      bundle->dt.requestTemperatures();
      float t = bundle->dt.getTempCByIndex(0);
      if (t != DEVICE_DISCONNECTED_C && s.pinTemp[0]) write(s.pinTemp, t);
    }
#endif
  }
}

// ── OTA ───────────────────────────────────────────────────────────────────────
void FluxgridClass::enableOTA(const char *password, const char *hostname) {
  _otaEnabled = true;
  String host = hostname ? String(hostname) : (String("fluxgrid-") + String(_token).substring(0, 6));
  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.setPassword(password);
  // begin() is called after WiFi connects, inside connectCloudOnce
}

// ── Connection ────────────────────────────────────────────────────────────────
String FluxgridClass::base() const { return String("fluxgrid/") + _token; }

void FluxgridClass::begin(const char *ssid, const char *pass, const char *token, const char *mqttUser, const char *mqttPass) {
  setDevice(token, mqttUser, mqttPass);
  begin(ssid, pass);
}

void FluxgridClass::begin(const char *ssid, const char *pass) {
  _ssid = ssid;
  _pass = pass;
  _started = true;

#if FLUXGRID_DEBUG
  // Make sure the serial monitor is alive so the user actually sees the logs.
  if (!Serial) Serial.begin(FLUXGRID_DEBUG_BAUD);
#endif
  FG_LOG("Fluxgrid library v%s", FLUXGRID_VERSION);
  FG_LOG("debug on — starting (token=%s, host=%s:%u, tls=%s)",
         (_token && _token[0]) ? _token : "(none)",
         _host, _port, _secure ? (_caCert ? "yes, CA pinned" : "yes, unverified") : "no");

  // Catch the most common zero-arg mistake: credentials #defined AFTER the
  // include (so the macros were still empty when begin() expanded).
  if (!_token || _token[0] == '\0')
    Serial.println(F("[Fluxgrid] empty device token — #define FG_TOKEN BEFORE #include <Fluxgrid.h>"));

  if (_secure) {
    // With a pinned CA the device refuses to talk to an impostor broker.
    // Without one we still encrypt but skip verification — beginner-friendly
    // default (no cert to paste), MITM-able by an active attacker. See
    // setCACert() in the header.
    if (_caCert) _netSecure.setCACert(_caCert);
    else         _netSecure.setInsecure();
    _mqtt.setClient(_netSecure);
  } else {
    _mqtt.setClient(_net);
  }
  _mqtt.setServer(_host, _port);
  _mqtt.setCallback(_fg_trampoline);
  _mqtt.setBufferSize(512);
  _mqtt.setKeepAlive(30);

  connectWiFi();
  connectCloudOnce();
}

void FluxgridClass::connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  FG_LOG("WiFi: connecting to \"%s\" ...", _ssid ? _ssid : "");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true); // let the core re-associate on drops on its own
  WiFi.begin(_ssid, _pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED)
    FG_LOG("WiFi: connected, IP %s", WiFi.localIP().toString().c_str());
  else
    FG_LOG("WiFi: connect timed out (check SSID/password)");
}

bool FluxgridClass::connectCloudOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String status   = base() + "/status";
  String clientId = String("fg-") + _token;
  FG_LOG("cloud: connecting to %s:%u as %s ...", _host, _port,
         (_mqttUser && _mqttUser[0]) ? _mqttUser : "(no user)");
  bool ok = _mqtt.connect(clientId.c_str(), _mqttUser, _key, status.c_str(), 0, true, "offline");
  if (ok) {
    // Presence payload carries the library version after the "online" keyword,
    // e.g. "online 0.9.3". The Last-Will is a bare "offline", and any reader
    // that only checks the first token still sees "online".
    String online = String("online ") + FLUXGRID_VERSION;
    _mqtt.publish(status.c_str(), online.c_str(), true);
    String wsub = base() + "/w/+";
    _mqtt.subscribe(wsub.c_str());
    FG_LOG("cloud: connected — online, subscribed %s", wsub.c_str());
    if (_acEnabled) {
      String cfgsub = base() + "/config";
      _mqtt.subscribe(cfgsub.c_str());
      FG_LOG("cloud: autoConfig on, subscribed %s", cfgsub.c_str());
    }
    if (_otaEnabled) ArduinoOTA.begin();
    _lastBeat = millis();
    if (_onConnected) _onConnected();
  } else {
    // PubSubClient state(): -4 timeout, -2 connect failed, 4 bad credentials, 5 unauthorized…
    FG_LOG("cloud: connect failed, state=%d (retrying)", _mqtt.state());
  }
  return ok;
}

void FluxgridClass::run() {
  if (!_started) return;
  if (WiFi.status() != WL_CONNECTED) {
    // Non-blocking reconnect. setAutoReconnect() (set in connectWiFi) lets the
    // core re-associate on its own; this is a fallback kick at most every 5s in
    // case it doesn't. We return immediately either way, so loop() keeps running
    // — the old code called the blocking connectWiFi() here and froze the sketch
    // for up to 20s on every WiFi dropout.
    unsigned long now = millis();
    if (now - _lastWifiTry >= 5000) {
      _lastWifiTry = now;
      FG_LOG("WiFi: link down — reconnecting (non-blocking)…");
      WiFi.begin(_ssid, _pass);
    }
    return;
  }
  if (!_mqtt.connected()) {
    unsigned long now = millis();
    if (now - _lastTry >= 1500) { _lastTry = now; connectCloudOnce(); }
    return;
  }
  _mqtt.loop();
  // Heartbeat: republish retained "online" every 30s. The Last-Will gives the
  // broker the *offline* edge on disconnect; this keeps presence fresh for
  // devices that send little/no telemetry, so the server's staleness sweep
  // never reaps a device that's actually connected.
  unsigned long now = millis();
  if (now - _lastBeat >= 30000) {
    _lastBeat = now;
    String status = base() + "/status";
    String online = String("online ") + FLUXGRID_VERSION;
    _mqtt.publish(status.c_str(), online.c_str(), true);
  }
  if (_otaEnabled) ArduinoOTA.handle();
  if (_acEnabled) runAutoConfig();
}

bool FluxgridClass::connected() { return _mqtt.connected(); }

// ── Write ─────────────────────────────────────────────────────────────────────
void FluxgridClass::write(const char *handle, const String &value) {
  if (!_mqtt.connected()) {
    FG_LOG("write: dropped %s = %s (not connected)", handle, value.c_str());
    return;
  }
  String topic = base() + "/v/" + handle;
  _mqtt.publish(topic.c_str(), value.c_str());
  FG_LOG("write: %s = %s", handle, value.c_str());
}
void FluxgridClass::write(const char *handle, const char *value) { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, int value)         { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, long value)        { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, float value)       { write(handle, String(value, 2)); }
void FluxgridClass::write(const char *handle, double value)      { write(handle, String(value, 2)); }
void FluxgridClass::write(const char *handle, bool value)        { write(handle, String(value ? 1 : 0)); }

// ── Serial capture: Fluxgrid.println(...) and the FLUXGRID_CAPTURE_SERIAL macro
// both funnel here, through the one shared tee (USB + "log" datastream). ───────
size_t FluxgridClass::write(uint8_t b)              { return FluxgridSerialPort.write(b); }
void   FluxgridClass::setLogHandle(const char *h)  { FluxgridSerialPort.setLogHandle(h); }

// ── Dispatch ──────────────────────────────────────────────────────────────────
void FluxgridClass::_dispatch(char *topic, uint8_t *payload, unsigned int len) {
  String t(topic);

  // Config topic: fluxgrid/<token>/config
  if (_acEnabled && t.endsWith("/config")) {
    if (len > 0) {
      String json;
      json.reserve(len);
      for (unsigned int i = 0; i < len; i++) json += (char)payload[i];
      FG_LOG("config: received %u bytes, applying", len);
      applyConfig(json);
      FG_LOG("config: applied (%u inputs, %u sensors, %u outputs, %u pwm)",
             _acInputCount, _acSensorCount, _acOutputCount, _acPwmCount);
    }
    return;
  }

  int slash = t.lastIndexOf('/');
  if (slash < 0) return;
  String pin = t.substring(slash + 1);

  String raw;
  raw.reserve(len);
  for (unsigned int i = 0; i < len; i++) raw += (char)payload[i];
  float val = raw.toFloat();

  FG_LOG("recv: %s = %s", pin.c_str(), raw.c_str());

  // Update cache first, then fire callback
  Slot *s = findOrCreateSlot(pin.c_str());
  if (s) {
    s->value = val;
    s->raw   = raw;
    s->has   = true;
    if (s->fn) s->fn(FluxValue(raw));
  }
}

// ── LED control ───────────────────────────────────────────────────────────────

FluxgridClass::LedEntry *FluxgridClass::_findLed(uint8_t gpio) {
  for (uint8_t i = 0; i < _ledCount; i++)
    if (_leds[i].gpio == gpio) return &_leds[i];
  return nullptr;
}

/*
  Send one WS2812 GRB frame over the ESP32 RMT peripheral.

  Timing (WS2812B datasheet, tolerated by most clones):
    Bit-0: 400 ns HIGH, 850 ns LOW
    Bit-1: 800 ns HIGH, 450 ns LOW
    Reset: >50 µs LOW (we send 60 µs)

  We clock RMT at 10 MHz (100 ns/tick) so all durations are multiples of 100 ns:
    Bit-0: 4 ticks HIGH, 8 ticks LOW
    Bit-1: 8 ticks HIGH, 4 ticks LOW
    Reset: 600 ticks LOW (handled by the idle level after transmission)
*/
void FluxgridClass::_ws2812Send(uint8_t gpio, uint8_t rmtIdx,
                                uint8_t r, uint8_t g, uint8_t b) {
  // WS2812 expects GRB bit order, MSB first
  uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  // ── Arduino core 3.x ─────────────────────────────────────────────────────
  rmt_data_t items[24];
  for (int i = 23; i >= 0; i--) {
    if (grb & (1u << i)) {
      items[23 - i].level0    = 1; items[23 - i].duration0 = 8;
      items[23 - i].level1    = 0; items[23 - i].duration1 = 4;
    } else {
      items[23 - i].level0    = 1; items[23 - i].duration0 = 4;
      items[23 - i].level1    = 0; items[23 - i].duration1 = 8;
    }
  }
  // Clock RMT at 10 MHz → 100 ns per tick (see durations above).
  rmtInit(gpio, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  rmtWrite(gpio, items, 24, RMT_WAIT_FOR_EVER);

#else
  // ── Arduino core 2.x (ESP-IDF v4 RMT driver) ─────────────────────────────
  rmt_channel_t ch = (rmt_channel_t)(rmtIdx & 0x07); // up to 8 TX channels
  static uint8_t initialised = 0;
  if (!(initialised & (1u << ch))) {
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)gpio, ch);
    cfg.clk_div = 8; // 80 MHz / 8 = 10 MHz → 100 ns per tick
    rmt_config(&cfg);
    rmt_driver_install(ch, 0, 0);
    initialised |= (1u << ch);
  }
  rmt_item32_t items[24];
  for (int i = 23; i >= 0; i--) {
    if (grb & (1u << i)) {
      items[23 - i].level0 = 1; items[23 - i].duration0 = 8;
      items[23 - i].level1 = 0; items[23 - i].duration1 = 4;
    } else {
      items[23 - i].level0 = 1; items[23 - i].duration0 = 4;
      items[23 - i].level1 = 0; items[23 - i].duration1 = 8;
    }
  }
  rmt_write_items(ch, items, 24, true);
  // Hold the line low for ≥50 µs reset pulse
  delayMicroseconds(60);
#endif
}

void FluxgridClass::addLed(uint8_t gpio, uint8_t type, uint8_t r, uint8_t g, uint8_t b) {
  if (_ledCount >= 8) return;
  LedEntry &e = _leds[_ledCount++];
  e.gpio = gpio;
  e.type = type;
  e.r    = r;
  e.g    = g;
  e.b    = b;
  if (type == LED_WS2812) {
    e.rmtIdx = _ws2812Count++;
    // Ensure the line starts low (reset state)
    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, LOW);
    delayMicroseconds(60);
  } else {
    e.rmtIdx = 0;
    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, LOW);
  }
  FG_LOG("addLed: gpio=%u type=%s", gpio, type == LED_WS2812 ? "WS2812" : "normal");
}

void FluxgridClass::ledOn(uint8_t gpio) {
  LedEntry *e = _findLed(gpio);
  if (!e) return;
  if (e->type == LED_WS2812) {
    _ws2812Send(gpio, e->rmtIdx, e->r, e->g, e->b);
  } else {
    digitalWrite(gpio, HIGH);
  }
}

void FluxgridClass::ledOn(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b) {
  LedEntry *e = _findLed(gpio);
  if (!e || e->type != LED_WS2812) return;
  e->r = r; e->g = g; e->b = b;
  _ws2812Send(gpio, e->rmtIdx, r, g, b);
}

void FluxgridClass::ledOff(uint8_t gpio) {
  LedEntry *e = _findLed(gpio);
  if (!e) return;
  if (e->type == LED_WS2812) {
    _ws2812Send(gpio, e->rmtIdx, 0, 0, 0);
  } else {
    digitalWrite(gpio, LOW);
  }
}
