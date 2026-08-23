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

// ── File explorer transport ───────────────────────────────────────────────────
// HTTPClient drives the presigned MinIO transfers (pull = PUT, push = GET).
// FS.h and WiFiClientSecure.h come in via Fluxgrid.h. No concrete filesystem
// header is needed here — addVolume takes an fs::FS& and the sketch supplies the
// concrete includes (LittleFS / SD / SD_MMC / FFat).
#include <HTTPClient.h>

// ── Device-info reporting (the retained `meta` topic) ─────────────────────────
// ESP-IDF headers behind buildMetaPayload(): the chip-feature bitmask
// (esp_chip_info), the per-interface MAC reader (esp_read_mac / ESP_MAC_BT) and
// the reset-reason enum (esp_reset_reason). They ship with the ESP32 Arduino
// core; ESP.* (heap/flash/PSRAM/efuse) comes in via Arduino.h in Fluxgrid.h.
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_system.h>

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

// Region -> broker host. One cloud per region; a device's credentials live on
// exactly one of them (see region() in Fluxgrid.h). Unknown codes leave the
// default in place rather than guessing, so a typo can't silently point a
// device at the wrong cloud.
void FluxgridClass::region(const char *code) {
  if (!code) return;
  if      (!strcmp(code, "com")) _host = "mqtt.lonelybinary.com";
  else if (!strcmp(code, "cn"))  _host = "mqtt.lonelybinary.cn";
  else {
    FG_LOG("region: unknown code \"%s\" - keeping %s", code, _host);
    return;
  }
  _port = 8883;
  FG_LOG("region: %s -> %s:%u", code, _host, _port);
}
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

// ── Local bridge hooks ──────────────────────────────────────────────────────
void FluxgridClass::onWrite(std::function<void(const char *, const String &)> fn) {
  _onWrite = fn;
}

void FluxgridClass::_applyIncoming(const char *pin, const String &raw) {
  Slot *s = findOrCreateSlot(pin);
  if (s) {
    s->value = raw.toFloat();
    s->raw   = raw;
    s->has   = true;
    if (s->fn) s->fn(FluxValue(raw));
  }
}

void FluxgridClass::inject(const char *pin, const String &value) {
  FG_LOG("inject: %s = %s", pin, value.c_str());
  _applyIncoming(pin, value);
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
  if (_cloud && (!_token || _token[0] == '\0'))
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
  // Big enough to hold a full WiFi-scan payload (a JSON array of ~16 networks)
  // alongside the topic and MQTT framing. The whole packet must fit this one
  // buffer, so writeWiFiScan() trims its output to getBufferSize() at runtime.
  // Raise it from your sketch with `#define FLUXGRID_MQTT_BUFFER <bytes>` before
  // #include <Fluxgrid.h> when you send/receive larger payloads (e.g. the
  // Jukebox streams whole songs and uses 8 KB).
  _mqtt.setBufferSize(FLUXGRID_MQTT_BUFFER);
  _mqtt.setKeepAlive(30);

  connectWiFi();
  if (_cloud) connectCloudOnce();
}

void FluxgridClass::manageWiFi(bool on) { _manageWifi = on; }
void FluxgridClass::cloud(bool on)      { _cloud = on; }

void FluxgridClass::startAP(const char *ssid, const char *pass) {
  // AP+STA so a managed (or your own) STA link — and the cloud — keeps working
  // alongside the access point. Note the ESP32 shares one radio: AP and STA end
  // up on the same channel, so a STA reconnect can briefly bump AP clients.
  WiFi.mode(WIFI_AP_STA);
  bool ok = (pass && pass[0]) ? WiFi.softAP(ssid, pass) : WiFi.softAP(ssid);
  _apOn = ok;
  FG_LOG("AP: \"%s\" %s — http://%s/", ssid, ok ? "up" : "FAILED",
         WiFi.softAPIP().toString().c_str());
}

void FluxgridClass::stopAP() {
  WiFi.softAPdisconnect(true);
  _apOn = false;
  FG_LOG("AP: stopped");
}

void FluxgridClass::connectWiFi() {
  if (!_manageWifi) return;            // the sketch owns the radio — hands off
  if (WiFi.status() == WL_CONNECTED) return;
  FG_LOG("WiFi: connecting to \"%s\" ...", _ssid ? _ssid : "");
  WiFi.mode(_apOn ? WIFI_AP_STA : WIFI_STA);  // don't clobber an AP we brought up
  WiFi.setAutoReconnect(true); // let the core re-associate on drops on its own
  WiFi.begin(_ssid, _pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED)
    FG_LOG("WiFi: connected, IP %s", WiFi.localIP().toString().c_str());
  else
    FG_LOG("WiFi: connect timed out (check SSID/password)");
}

// Build the retained `meta` payload published on every cloud connect: the joined
// SSID at the top level (read by the dashboard for the RSSI widget) plus an
// `info` block of static + live device facts for the Device Info widget. Built
// with ArduinoJson so strings (SSID, MACs) are escaped for us; serialized into a
// String the caller publishes. The 1024-byte document comfortably fits the shape
// below and serializes well under the 2048-byte MQTT buffer.
String FluxgridClass::buildMetaPayload() {
  StaticJsonDocument<1024> doc;

  // Top level — the backend still reads the SSID here.
  doc["ssid"] = WiFi.SSID();

  JsonObject info = doc.createNestedObject("info");

  // chip — model / revision / cores / clock / efuse id / silicon features.
  JsonObject chip = info.createNestedObject("chip");
  chip["model"] = ESP.getChipModel();
  chip["rev"]   = String("v") + ESP.getChipRevision();   // e.g. "v3"
  chip["cores"] = ESP.getChipCores();
  chip["mhz"]   = ESP.getCpuFreqMHz();
  // efuse MAC is a 48-bit value in the low 6 bytes; print them MSB-first as
  // uppercase hex (e.g. "ECDA3B1A2B3C"), matching mac.sta with the colons removed.
  uint64_t efuse = ESP.getEfuseMac();
  char idHex[13];
  snprintf(idHex, sizeof(idHex), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(efuse >> 40), (uint8_t)(efuse >> 32), (uint8_t)(efuse >> 24),
           (uint8_t)(efuse >> 16), (uint8_t)(efuse >> 8),  (uint8_t)(efuse));
  chip["id"] = idHex;
  // Feature bitmask → short labels. Guarded with #ifdef as not every core/target
  // defines every CHIP_FEATURE_* constant.
  JsonArray features = chip.createNestedArray("features");
  esp_chip_info_t ci;
  esp_chip_info(&ci);
#ifdef CHIP_FEATURE_WIFI_BGN
  if (ci.features & CHIP_FEATURE_WIFI_BGN) features.add("WiFi");
#endif
#ifdef CHIP_FEATURE_BLE
  if (ci.features & CHIP_FEATURE_BLE)      features.add("BLE");
#endif
#ifdef CHIP_FEATURE_BT
  if (ci.features & CHIP_FEATURE_BT)       features.add("BT");
#endif
#ifdef CHIP_FEATURE_EMB_FLASH
  if (ci.features & CHIP_FEATURE_EMB_FLASH) features.add("EmbFlash");
#endif
#ifdef CHIP_FEATURE_EMB_PSRAM
  if (ci.features & CHIP_FEATURE_EMB_PSRAM) features.add("EmbPSRAM");
#endif

  // mem — total heap / flash / PSRAM in bytes (psram = 0 when none fitted).
  JsonObject mem = info.createNestedObject("mem");
  mem["heap"]  = ESP.getHeapSize();
  mem["flash"] = ESP.getFlashChipSize();
  mem["psram"] = ESP.getPsramSize();

  // mac — station, soft-AP and Bluetooth interface addresses.
  JsonObject mac = info.createNestedObject("mac");
  mac["sta"] = WiFi.macAddress();
  mac["ap"]  = WiFi.softAPmacAddress();
  // BT MAC isn't on the WiFi API; read it straight from efuse. Best-effort —
  // only emit the field if the read succeeds.
  uint8_t btmac[6];
  if (esp_read_mac(btmac, ESP_MAC_BT) == ESP_OK) {
    char btStr[18];
    snprintf(btStr, sizeof(btStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             btmac[0], btmac[1], btmac[2], btmac[3], btmac[4], btmac[5]);
    mac["bt"] = btStr;
  }

  // fw — SDK version string + why the chip last reset.
  JsonObject fw = info.createNestedObject("fw");
  fw["sdk"] = ESP.getSdkVersion();
  const char *reset;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   reset = "POWERON";   break;
    case ESP_RST_SW:        reset = "SW";        break;
    case ESP_RST_PANIC:     reset = "PANIC";     break;
    case ESP_RST_INT_WDT:   reset = "INT_WDT";   break;
    case ESP_RST_TASK_WDT:  reset = "TASK_WDT";  break;
    case ESP_RST_WDT:       reset = "WDT";       break;
    case ESP_RST_DEEPSLEEP: reset = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT:  reset = "BROWNOUT";  break;
    case ESP_RST_SDIO:      reset = "SDIO";      break;
    default:                reset = "UNKNOWN";   break;
  }
  fw["reset"] = reset;

  // net — the live link: address, gateway, DNS, signal, channel, BSSID.
  JsonObject net = info.createNestedObject("net");
  net["ip"]   = WiFi.localIP().toString();
  net["mask"] = WiFi.subnetMask().toString();
  net["gw"]   = WiFi.gatewayIP().toString();
  JsonArray dns = net.createNestedArray("dns");
  for (int i = 0; i < 2; i++) {
    IPAddress d = WiFi.dnsIP(i);
    if (d != IPAddress(0, 0, 0, 0)) dns.add(d.toString());   // skip unset slots
  }
  net["rssi"]  = WiFi.RSSI();
  net["ch"]    = WiFi.channel();
  net["bssid"] = WiFi.BSSIDstr();

  String out;
  serializeJson(doc, out);
  return out;
}

// Periodic memory snapshot for reportMemory(). One compact JSON object carries
// every value the Memory Chart widget needs, so the whole update is a single
// MQTT publish rather than five separate telemetry writes. Heap is always
// present; the PSRAM pair is emitted only when the board actually has PSRAM
// (ESP.getPsramSize() > 0), and the widget hides that half when it's absent.
//   fh  = free_heap      mfh = min_free_heap   ma = max_alloc
//   fp  = free_psram     mfp = min_free_psram
String FluxgridClass::buildMemPayload() {
  StaticJsonDocument<192> doc;
  doc["fh"]  = ESP.getFreeHeap();
  doc["mfh"] = ESP.getMinFreeHeap();
  doc["ma"]  = ESP.getMaxAllocHeap();
  if (ESP.getPsramSize() > 0) {
    doc["fp"]  = ESP.getFreePsram();
    doc["mfp"] = ESP.getMinFreePsram();
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void FluxgridClass::reportMemory(uint32_t intervalMs) {
  _reportMem   = intervalMs > 0;
  _memInterval = intervalMs > 0 ? intervalMs : _memInterval;
  _lastMem     = 0; // publish on the next run() so the chart fills immediately
  if (_reportMem) FG_LOG("reportMemory: on, every %ums", (unsigned)_memInterval);
  else            FG_LOG("reportMemory: off");
}

bool FluxgridClass::connectCloudOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String status   = base() + "/status";
  String clientId = String("fg-") + _token;
  FG_LOG("cloud: connecting to %s:%u as %s ...", _host, _port,
         (_mqttUser && _mqttUser[0]) ? _mqttUser : "(no user)");
  bool ok = _mqtt.connect(clientId.c_str(), _mqttUser, _key, status.c_str(), 0, true, "offline");
  if (ok) {
    // Subscribe to control writes BEFORE announcing "online". The cloud replays
    // the last stateful control values (switch/slider) the moment it sees this
    // device come online, so the subscription must already be in place or that
    // replay would race ahead of it and be lost. QoS 1 so the broker buffers
    // the replay to us rather than dropping it.
    String wsub = base() + "/w/+";
    _mqtt.subscribe(wsub.c_str(), 1);
    // Grouped control writes for multi-variable widgets (joystick / RGB / range
    // slider) arrive on wg/<group> as one JSON packet; subscribe alongside w/+.
    String wgsub = base() + "/wg/+";
    _mqtt.subscribe(wgsub.c_str(), 1);
    if (_acEnabled) {
      String cfgsub = base() + "/config";
      _mqtt.subscribe(cfgsub.c_str(), 1);
      FG_LOG("cloud: autoConfig on, subscribed %s", cfgsub.c_str());
    }
    if (_filesEnabled) {
      // File-explorer command channel. QoS 1 so a command the dashboard sends
      // while we're momentarily busy is buffered by the broker rather than lost.
      String fssub = base() + "/fs/req";
      _mqtt.subscribe(fssub.c_str(), 1);
      FG_LOG("cloud: file explorer on, subscribed %s", fssub.c_str());
    }
    // Presence payload carries the library version after the "online" keyword,
    // e.g. "online 0.9.3". The Last-Will is a bare "offline", and any reader
    // that only checks the first token still sees "online".
    String online = String("online ") + FLUXGRID_VERSION;
    _mqtt.publish(status.c_str(), online.c_str(), true);
    // Report device metadata once per connect — not on the heartbeat — on a
    // dedicated retained `meta` topic. The payload is a JSON object built with
    // ArduinoJson (so SSID spaces/quotes are escaped for us): the joined WiFi
    // `ssid` at the top level (the dashboard still reads it there for the RSSI
    // widget) plus a richer `info` block — chip model/revision/cores/id/features,
    // heap/flash/PSRAM sizes, STA/AP/BT MACs, SDK + reset reason, and the live
    // network (IP/mask/gateway/DNS/RSSI/channel/BSSID) — that drives the Device
    // Info widget. See buildMetaPayload(). Retained so the bridge still sees it
    // if it (re)subscribes after the device connected.
    String meta = base() + "/meta";
    String metaPayload = buildMetaPayload();
    _mqtt.publish(meta.c_str(), metaPayload.c_str(), true);
    FG_LOG("cloud: connected — subscribed %s, online (ssid=%s)", wsub.c_str(), WiFi.SSID().c_str());
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

  // (1) Radio: only WE manage it when manageWiFi(true) (the default). If the
  // sketch owns WiFi (manageWiFi(false)), we never touch the radio here — no
  // mode changes, no reconnect kicks — we only OBSERVE the link below.
  if (_manageWifi && WiFi.status() != WL_CONNECTED) {
    // Non-blocking reconnect kick (at most every 5s). setAutoReconnect() lets
    // the core re-associate on its own; this is a fallback. We never block, so
    // loop() keeps running — unlike the old code that froze for up to 20s.
    unsigned long now = millis();
    if (now - _lastWifiTry >= 5000) {
      _lastWifiTry = now;
      FG_LOG("WiFi: link down — reconnecting (non-blocking)…");
      WiFi.begin(_ssid, _pass);
    }
    return;   // no uplink yet — nothing to service
  }

  // (2) Cloud: only when enabled and an uplink actually exists. Observing
  // WiFi.status() is read-only; it never drives the radio.
  if (_cloud && WiFi.status() == WL_CONNECTED) {
    if (!_mqtt.connected()) {
      unsigned long now = millis();
      if (now - _lastTry >= 1500) { _lastTry = now; connectCloudOnce(); }
    } else {
      _mqtt.loop();
      // Heartbeat: republish retained "online" every 30s so the server's
      // staleness sweep never reaps a device that's actually connected.
      unsigned long now = millis();
      if (now - _lastBeat >= 30000) {
        _lastBeat = now;
        String status = base() + "/status";
        String online = String("online ") + FLUXGRID_VERSION;
        _mqtt.publish(status.c_str(), online.c_str(), true);
      }
      // Automatic memory snapshot (reportMemory): one non-retained `mem` message
      // carrying heap (+ PSRAM) so the Memory Chart widget stays live. Slow-moving
      // data, so the default 5s cadence is gentle on the broker.
      if (_reportMem && (_lastMem == 0 || now - _lastMem >= _memInterval)) {
        _lastMem = now;
        String mem = base() + "/mem";
        String payload = buildMemPayload();
        _mqtt.publish(mem.c_str(), payload.c_str(), false);
      }
    }
  }

  // (3) Housekeeping that doesn't depend on the cloud link.
  if (_otaEnabled) ArduinoOTA.handle();
  if (_acEnabled) runAutoConfig();
  // Publish file-transfer progress/result from THIS (main) thread — the
  // background task only writes to the shared struct, it never touches MQTT.
  if (_filesEnabled) _fsPollTransfer();
}

bool FluxgridClass::connected() { return _mqtt.connected(); }

// ── Write ─────────────────────────────────────────────────────────────────────
void FluxgridClass::write(const char *handle, const String &value) {
  // Tee to the local bridge first (FluxgridLocal SSE) — this fires even with no
  // cloud, so an offline/AP dashboard still updates.
  if (_onWrite) _onWrite(handle, value);

  if (_cloud && _mqtt.connected()) {
    String topic = base() + "/v/" + handle;
    _mqtt.publish(topic.c_str(), value.c_str());
    FG_LOG("write: %s = %s", handle, value.c_str());
  } else if (!_onWrite) {
    FG_LOG("write: dropped %s = %s (not connected)", handle, value.c_str());
  }
}
void FluxgridClass::write(const char *handle, const char *value) { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, int value)         { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, long value)        { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, unsigned int value)  { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, unsigned long value) { write(handle, String(value)); }
void FluxgridClass::write(const char *handle, float value)       { write(handle, String(value, 2)); }
void FluxgridClass::write(const char *handle, double value)      { write(handle, String(value, 2)); }
void FluxgridClass::write(const char *handle, bool value)        { write(handle, String(value ? 1 : 0)); }

// ── WiFi scan ───────────────────────────────────────────────────────────────
// Turn an ESP32 auth mode into a short human label for the payload / widget.
const char *FluxgridClass::_wifiEncName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA+WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2+WPA3";
    default:                        return "WPA2";
  }
}

// Serialize the completed scan (n results held by the WiFi driver) into the
// JSON the WiFi Scan widget reads and publish it to `handle`. Networks are
// sorted strongest-first, capped to maxNets, and trimmed so the whole MQTT
// packet stays inside the client buffer. Returns the count actually published.
//
//   [{"ssid":"NET","rssi":-62,"channel":4,"enc":"WPA2","bssid":"AA:BB:.."},…]
//
int FluxgridClass::_publishWiFiScan(const char *handle, int n, int maxNets) {
  if (n <= 0) { write(handle, "[]"); return 0; }   // publish empty so the widget clears

  // Bound the working set so the index array stays a small stack allocation.
  const int HARD = 64;
  if (n > HARD) n = HARD;
  int idx[HARD];
  for (int i = 0; i < n; i++) idx[i] = i;

  // Partial selection sort: only the top `want` entries need ordering.
  int want = (maxNets > 0 && maxNets < n) ? maxNets : n;
  for (int i = 0; i < want; i++) {
    int best = i;
    for (int j = i + 1; j < n; j++)
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[best])) best = j;
    int t = idx[i]; idx[i] = idx[best]; idx[best] = t;
  }

  // Keep the whole packet (topic + payload + framing) within the MQTT buffer.
  String topic = base() + "/v/" + handle;
  int budget = (int)_mqtt.getBufferSize() - (int)topic.length() - 16;

  String payload = "[";
  int count = 0;
  for (int i = 0; i < want; i++) {
    int k = idx[i];
    String ssid = WiFi.SSID(k);
    ssid.replace("\\", "\\\\");      // escape backslashes first…
    ssid.replace("\"", "\\\"");      // …then double-quotes
    String entry = (count ? "," : "");
    entry += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(k)
           + ",\"channel\":" + WiFi.channel(k)
           + ",\"enc\":\"" + _wifiEncName(WiFi.encryptionType(k))
           + "\",\"bssid\":\"" + WiFi.BSSIDstr(k) + "\"}";
    if ((int)(payload.length() + entry.length() + 1) > budget) break; // +1 for the closing ]
    payload += entry;
    count++;
  }
  payload += "]";

  write(handle, payload);
  return count;
}

int FluxgridClass::writeWiFiScan(const char *handle, int maxNets) {
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  if (n < 0) return n;            // scan failed
  int count = _publishWiFiScan(handle, n, maxNets);
  WiFi.scanDelete();
  return count;
}

void FluxgridClass::startWiFiScan() {
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
}

int FluxgridClass::pollWiFiScan(const char *handle, int maxNets) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1; // still scanning — try again next loop()
  if (n == WIFI_SCAN_FAILED)  return -2; // no scan running / failed
  int count = _publishWiFiScan(handle, n, maxNets);
  WiFi.scanDelete();
  return count;
}

// ── Serial capture: Fluxgrid.println(...) and the FLUXGRID_CAPTURE_SERIAL macro
// both funnel here, through the one shared tee (USB + "log" datastream). ───────
size_t FluxgridClass::write(uint8_t b)              { return FluxgridSerialPort.write(b); }
void   FluxgridClass::setLogHandle(const char *h)  { FluxgridSerialPort.setLogHandle(h); }

// ── Dispatch ──────────────────────────────────────────────────────────────────
void FluxgridClass::_dispatch(char *topic, uint8_t *payload, unsigned int len) {
  String t(topic);

  // File-explorer command: fluxgrid/<token>/fs/req. Handled (and replied to)
  // synchronously here on the main loop — see _fsDispatch. Bulk transfers spawn
  // a background task there but still return immediately.
  if (_filesEnabled && t.endsWith("/fs/req")) {
    _fsDispatch((const char *)payload, len);
    return;
  }

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

  // Grouped write: fluxgrid/<token>/wg/<group> carries a JSON object whose keys
  // are sub-handles, e.g. {"x":0.45,"y":-0.32}. Fan each member out through the
  // same path as an individual write, so read()/onReceive() see them exactly as
  // before — the grouping is transparent to your sketch.
  if (t.indexOf("/wg/") >= 0) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload, len) == DeserializationError::Ok) {
      for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        String val = kv.value().as<String>();
        FG_LOG("recv(group): %s = %s", kv.key().c_str(), val.c_str());
        _applyIncoming(kv.key().c_str(), val);
      }
    }
    return;
  }

  int slash = t.lastIndexOf('/');
  if (slash < 0) return;
  String pin = t.substring(slash + 1);

  String raw;
  raw.reserve(len);
  for (unsigned int i = 0; i < len; i++) raw += (char)payload[i];

  FG_LOG("recv: %s = %s", pin.c_str(), raw.c_str());
  _applyIncoming(pin.c_str(), raw);   // update cache + fire onReceive (shared with inject)
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

// ── File explorer ──────────────────────────────────────────────────────────────
//
// Two transports (see src/lib/fs/protocol.ts — the cloud side this must match):
//   • metadata ops (volumes/list/stat/mkdir/delete/rename/format) ride MQTT as
//     small JSON on fluxgrid/<token>/fs/{req,res}. They run synchronously inside
//     _dispatch (the MQTT callback, on the main loop) and publish their reply
//     right there — fast and small, so they don't stall loop().
//   • file BYTES never touch MQTT: pull/push stream over HTTPS to/from presigned
//     MinIO URLs on a dedicated FreeRTOS task (core 0). That task touches only
//     HTTP + the filesystem; run() polls a mutex-guarded struct and publishes
//     progress/result from the main thread (PubSubClient is single-threaded).
//
// Every request echoes its `id`. Responses fit the 2048-byte MQTT buffer; `list`
// paginates with a numeric `cursor`/`next` and also stops early on the budget.

// JSON-escape one string value into `out` (no surrounding quotes added). Matches
// the escaping already used for the SSID in connectCloudOnce / the WiFi scan.
static void _fgJsonEscape(const String &in, String &out) {
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {            // other control chars → \u00XX
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
}

// Join a directory path and a leaf into a clean absolute path ("/" + "a" → "/a",
// "/d" + "a" → "/d/a"). Used when recursing into / building child paths.
static String _fgJoin(const String &dir, const char *leaf) {
  String p = dir;
  if (!p.endsWith("/")) p += "/";
  p += leaf;
  return p;
}

// Stream wrapper that counts bytes as HTTPClient pulls them from a File during a
// PUT (pull). HTTPClient streams via readBytes(); we tally there so the main
// thread can report progress without HTTPClient exposing a callback. RAM stays
// bounded — HTTPClient reads in its own small chunks, we never buffer the file.
class _FgCountingFile : public Stream {
public:
  _FgCountingFile(fs::File &f, volatile uint64_t *counter, volatile bool *progress,
                  portMUX_TYPE *mux)
    : _f(f), _counter(counter), _progress(progress), _mux(mux) {}
  int    available() override { return _f.available(); }
  int    read() override      { int c = _f.read(); if (c >= 0) _bump(1); return c; }
  int    peek() override      { return _f.peek(); }
  // Virtual in the ESP32 core's Stream — HTTPClient::sendRequest(Stream*) streams
  // the upload through here, so this is where we tally PUT progress.
  size_t readBytes(char *buffer, size_t length) override {
    size_t n = _f.readBytes(buffer, length);
    _bump(n);
    return n;
  }
  // Stream is also a Print; we never write to it, but the pure virtual must exist.
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t *, size_t) override { return 0; }
private:
  void _bump(size_t n) {
    if (!n) return;
    portENTER_CRITICAL(_mux);
    *_counter  += n;
    *_progress  = true;   // let the main thread emit a progress sample
    portEXIT_CRITICAL(_mux);
  }
  fs::File          &_f;
  volatile uint64_t *_counter;
  volatile bool     *_progress;
  portMUX_TYPE      *_mux;
};

const char *FluxgridClass::_volTypeName(uint8_t type) {
  switch (type) {
    case FG_FLASH: return "flash";
    case FG_SD:    return "sd";
    default:       return "other";
  }
}

void FluxgridClass::enableFiles() {
  _filesEnabled = true;
  FG_LOG("files: enabled (%u volume%s registered)", _volCount, _volCount == 1 ? "" : "s");
}

void FluxgridClass::_addVolume(const char *id, fs::FS *fs, uint8_t type,
                               const char *label, bool readonly,
                               std::function<uint64_t()> totalFn,
                               std::function<uint64_t()> usedFn,
                               std::function<bool()> formatFn) {
  if (!id || !id[0] || !fs) { FG_LOG("addVolume: bad id/fs — ignored"); return; }
  if (_volCount >= FLUXGRID_MAX_VOLUMES) {
    FG_LOG("addVolume: no slots left (max %d) — \"%s\" ignored", FLUXGRID_MAX_VOLUMES, id);
    return;
  }
  if (_findVolume(id)) { FG_LOG("addVolume: \"%s\" already registered — ignored", id); return; }
  Volume &v = _volumes[_volCount++];
  strncpy(v.id, id, sizeof(v.id) - 1); v.id[sizeof(v.id) - 1] = '\0';
  const char *lbl = (label && label[0]) ? label : id;
  strncpy(v.label, lbl, sizeof(v.label) - 1); v.label[sizeof(v.label) - 1] = '\0';
  v.type     = type;
  v.readonly = readonly;
  v.fs       = fs;
  v.totalFn  = totalFn;
  v.usedFn   = usedFn;
  v.formatFn = formatFn;
  FG_LOG("addVolume: \"%s\" (%s%s)", v.id, _volTypeName(type), readonly ? ", ro" : "");
}

// Escape-hatch overload (any fs::FS + caller-supplied capacity getters).
void FluxgridClass::addVolume(const char *id, fs::FS &fs, uint8_t type,
                              std::function<uint64_t()> totalFn,
                              std::function<uint64_t()> usedFn,
                              const char *label, bool readonly) {
  _addVolume(id, &fs, type, label, readonly, totalFn, usedFn, /*formatFn=*/nullptr);
}

FluxgridClass::Volume *FluxgridClass::_findVolume(const char *id) {
  if (!id) return nullptr;
  for (uint8_t i = 0; i < _volCount; i++)
    if (strcmp(_volumes[i].id, id) == 0) return &_volumes[i];
  return nullptr;
}

// Path safety (defence in depth — the cloud sanitises too): must be non-empty,
// start with '/', and contain no "..", no backslash, no NUL/control chars.
bool FluxgridClass::_fsPathOk(const char *p) {
  if (!p || p[0] != '/') return false;
  size_t n = strlen(p);
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\\') return false;
    if ((uint8_t)c < 0x20) return false;
    if (c == '.' && p[i + 1] == '.') {            // ".." as a path segment
      char prev = (i == 0) ? '/' : p[i - 1];
      char next = p[i + 2];
      if ((prev == '/') && (next == '/' || next == '\0')) return false;
    }
  }
  return true;
}

// ── Reply helpers (always from the main thread) ────────────────────────────────
void FluxgridClass::_fsPublish(const String &payload) {
  if (!_mqtt.connected()) return;
  String topic = base() + "/fs/res";
  bool ok = _mqtt.publish(topic.c_str(), payload.c_str());
  if (!ok) FG_LOG("files: publish failed (%u bytes vs buffer %u)",
                  (unsigned)payload.length(), (unsigned)_mqtt.getBufferSize());
}

void FluxgridClass::_fsReplyOk(const char *id) {
  String s = "{\"id\":\""; _fgJsonEscape(String(id), s); s += "\",\"ok\":true}";
  _fsPublish(s);
}

void FluxgridClass::_fsReplyErr(const char *id, const char *err) {
  String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
  s += "\",\"ok\":false,\"err\":\""; s += err; s += "\"}";
  FG_LOG("files: %s → %s", id, err);
  _fsPublish(s);
}

// ── Request router ─────────────────────────────────────────────────────────────
void FluxgridClass::_fsDispatch(const char *json, unsigned int len) {
  StaticJsonDocument<512> doc;   // requests are tiny (id + cmd + a path/url)
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    FG_LOG("files: bad request JSON");
    return; // no id to echo — drop
  }
  const char *id  = doc["id"]  | "";
  const char *cmd = doc["cmd"] | "";
  if (!id[0] || !cmd[0]) return;
  FG_LOG("files: cmd=%s id=%s", cmd, id);

  // volumes is the only command without a `vol`.
  if (strcmp(cmd, "volumes") == 0) { _fsVolumes(id); return; }

  const char *volId = doc["vol"] | "";
  Volume *v = _findVolume(volId);
  if (!v) { _fsReplyErr(id, "EOFFLINE"); return; }   // unknown / unmounted volume

  if (strcmp(cmd, "list") == 0) {
    const char *path = doc["path"] | "/";
    int limit = doc["limit"] | 50;
    const char *cursor = doc["cursor"] | (const char *)nullptr;
    _fsList(id, v, path, limit, cursor);
  } else if (strcmp(cmd, "stat") == 0) {
    _fsStat(id, v, doc["path"] | "/");
  } else if (strcmp(cmd, "mkdir") == 0) {
    if (v->readonly) { _fsReplyErr(id, "EACCES"); return; }
    _fsMkdir(id, v, doc["path"] | "");
  } else if (strcmp(cmd, "delete") == 0) {
    if (v->readonly) { _fsReplyErr(id, "EACCES"); return; }
    _fsDelete(id, v, doc["path"] | "");
  } else if (strcmp(cmd, "rename") == 0) {
    if (v->readonly) { _fsReplyErr(id, "EACCES"); return; }
    _fsRename(id, v, doc["path"] | "", doc["to"] | "");
  } else if (strcmp(cmd, "format") == 0) {
    if (v->readonly) { _fsReplyErr(id, "EACCES"); return; }
    _fsFormat(id, v);
  } else if (strcmp(cmd, "pull") == 0) {
    _fsStartTransfer(XFER_PULL, id, v, doc["path"] | "", doc["url"] | "", 0);
  } else if (strcmp(cmd, "push") == 0) {
    if (v->readonly) { _fsReplyErr(id, "EACCES"); return; }
    uint64_t size = doc["size"] | (uint64_t)0;
    _fsStartTransfer(XFER_PUSH, id, v, doc["path"] | "", doc["url"] | "", size);
  } else {
    _fsReplyErr(id, "EUNSUP");
  }
}

// ── volumes ────────────────────────────────────────────────────────────────────
void FluxgridClass::_fsVolumes(const char *id) {
  String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
  s += "\",\"ok\":true,\"volumes\":[";
  for (uint8_t i = 0; i < _volCount; i++) {
    Volume &v = _volumes[i];
    uint64_t total = v.totalFn ? v.totalFn() : 0;
    uint64_t used  = v.usedFn  ? v.usedFn()  : 0;
    if (i) s += ",";
    s += "{\"id\":\"";    _fgJsonEscape(String(v.id), s);
    s += "\",\"label\":\""; _fgJsonEscape(String(v.label), s);
    s += "\",\"type\":\""; s += _volTypeName(v.type);
    s += "\",\"total\":";  s += String((double)total, 0);
    s += ",\"used\":";     s += String((double)used, 0);
    s += ",\"online\":true,\"readonly\":"; s += v.readonly ? "true" : "false";
    s += "}";
  }
  s += "]}";
  _fsPublish(s);
}

// ── list (paginated; fits the MQTT buffer) ─────────────────────────────────────
void FluxgridClass::_fsList(const char *id, Volume *v, const char *path,
                            int limit, const char *cursor) {
  if (!_fsPathOk(path)) { _fsReplyErr(id, "EINVAL"); return; }
  if (limit < 1)  limit = 1;
  if (limit > 50) limit = 50;       // hard cap regardless of what the cloud asks
  uint32_t start = 0;
  if (cursor && cursor[0]) start = (uint32_t)strtoul(cursor, nullptr, 10);

  fs::File dir = v->fs->open(path);
  if (!dir) { _fsReplyErr(id, "ENOENT"); return; }
  if (!dir.isDirectory()) { dir.close(); _fsReplyErr(id, "EINVAL"); return; }

  // Header first so the running length is accurate against the budget.
  String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
  s += "\",\"ok\":true,\"path\":\""; _fgJsonEscape(String(path), s);
  s += "\",\"entries\":[";

  // Leave room for the entries-close, an optional "next" cursor, and the object
  // close, all within the device's MQTT send buffer.
  const int budget = (int)_mqtt.getBufferSize() - 64;

  uint32_t index = 0;     // position within the directory stream
  int      emitted = 0;
  bool     more = false;
  uint32_t nextIndex = 0;

  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    uint32_t cur = index++;
    if (cur < start) { entry.close(); continue; }   // resume past the cursor
    if (emitted >= limit) { more = true; nextIndex = cur; entry.close(); break; }

    // Build this entry, then commit only if it still fits the budget.
    String name = entry.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);   // leaf only
    bool isDir = entry.isDirectory();
    String e = (emitted ? "," : "");
    e += "{\"name\":\""; _fgJsonEscape(name, e);
    e += "\",\"size\":"; e += String((double)(isDir ? 0 : entry.size()), 0);
    e += ",\"dir\":"; e += isDir ? "true" : "false";
    time_t mt = entry.getLastWrite();
    if (mt > 0) { e += ",\"mtime\":"; e += String((double)mt, 0); }
    e += "}";

    if ((int)(s.length() + e.length()) > budget) {      // would overflow → page here
      more = true; nextIndex = cur;
      entry.close();
      break;
    }
    s += e;
    emitted++;
    entry.close();
  }
  dir.close();

  s += "]";
  if (more) { s += ",\"next\":\""; s += String(nextIndex); s += "\""; }
  s += "}";
  _fsPublish(s);
}

// ── stat ───────────────────────────────────────────────────────────────────────
void FluxgridClass::_fsStat(const char *id, Volume *v, const char *path) {
  if (!_fsPathOk(path)) { _fsReplyErr(id, "EINVAL"); return; }
  fs::File f = v->fs->open(path);
  if (!f) { _fsReplyErr(id, "ENOENT"); return; }
  bool isDir = f.isDirectory();
  uint32_t size = isDir ? 0 : f.size();
  time_t mt = f.getLastWrite();
  String name = f.name();
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  f.close();

  String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
  s += "\",\"ok\":true,\"name\":\""; _fgJsonEscape(name, s);
  s += "\",\"size\":"; s += String((double)size, 0);
  s += ",\"dir\":"; s += isDir ? "true" : "false";
  if (mt > 0) { s += ",\"mtime\":"; s += String((double)mt, 0); }
  s += "}";
  _fsPublish(s);
}

// ── mkdir ──────────────────────────────────────────────────────────────────────
void FluxgridClass::_fsMkdir(const char *id, Volume *v, const char *path) {
  if (!_fsPathOk(path) || !path[1]) { _fsReplyErr(id, "EINVAL"); return; }
  if (v->fs->exists(path)) { _fsReplyErr(id, "EEXIST"); return; }
  if (v->fs->mkdir(path)) _fsReplyOk(id);
  else                    _fsReplyErr(id, "EIO");
}

// ── delete (file, or directory recursively) ────────────────────────────────────
void FluxgridClass::_fsDelete(const char *id, Volume *v, const char *path) {
  if (!_fsPathOk(path) || !path[1]) { _fsReplyErr(id, "EINVAL"); return; }   // never the root
  if (!v->fs->exists(path)) { _fsReplyErr(id, "ENOENT"); return; }
  fs::File f = v->fs->open(path);
  bool isDir = f && f.isDirectory();
  f.close();
  bool ok;
  if (isDir) ok = _fsRmRf(v->fs, String(path), /*keepRoot=*/false);
  else       ok = v->fs->remove(path);
  if (ok) _fsReplyOk(id);
  else    _fsReplyErr(id, "EIO");
}

// Recursively remove everything under `path`. With keepRoot the directory at
// `path` itself is kept (used for SD "format" at root); otherwise it's rmdir'd
// once emptied. Iterative-per-level recursion; directory depth is bounded in
// practice on these filesystems.
bool FluxgridClass::_fsRmRf(fs::FS *fs, const String &path, bool keepRoot) {
  fs::File dir = fs->open(path);
  if (!dir) return false;
  if (!dir.isDirectory()) { dir.close(); return fs->remove(path); }

  bool ok = true;
  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    String child = entry.name();           // may be a leaf or a full path
    if (child.indexOf('/') < 0) child = _fgJoin(path, child.c_str());
    bool childDir = entry.isDirectory();
    entry.close();
    if (childDir) ok = _fsRmRf(fs, child, /*keepRoot=*/false) && ok;
    else          ok = fs->remove(child) && ok;
  }
  dir.close();
  if (!keepRoot) ok = fs->rmdir(path) && ok;
  return ok;
}

// ── rename ─────────────────────────────────────────────────────────────────────
void FluxgridClass::_fsRename(const char *id, Volume *v, const char *path, const char *to) {
  if (!_fsPathOk(path) || !_fsPathOk(to) || !path[1] || !to[1]) { _fsReplyErr(id, "EINVAL"); return; }
  if (!v->fs->exists(path)) { _fsReplyErr(id, "ENOENT"); return; }
  if (v->fs->exists(to))    { _fsReplyErr(id, "EEXIST"); return; }
  if (v->fs->rename(path, to)) _fsReplyOk(id);
  else                         _fsReplyErr(id, "EIO");
}

// ── format (flash: real format · SD/other: wipe everything at root) ────────────
void FluxgridClass::_fsFormat(const char *id, Volume *v) {
  bool ok;
  if (v->formatFn) {
    FG_LOG("files: formatting volume \"%s\" (real format)", v->id);
    ok = v->formatFn();
  } else {
    // No portable format for SD — wipe everything under root instead.
    FG_LOG("files: wiping volume \"%s\" (no format — deleting root contents)", v->id);
    ok = _fsRmRf(v->fs, String("/"), /*keepRoot=*/true);
  }
  if (ok) _fsReplyOk(id);
  else    _fsReplyErr(id, "EIO");
}

// ── Bulk transfers (pull/push) ─────────────────────────────────────────────────
void FluxgridClass::_fsPrepSecure(WiFiClientSecure &c) {
  // Mirror the MQTT TLS policy: pin the broker CA if the sketch set one,
  // otherwise accept the cert unverified. The presigned URL is unguessable and
  // short-lived, so an unverified TLS link is an acceptable default here.
  if (_caCert) c.setCACert(_caCert);
  else         c.setInsecure();
}

void FluxgridClass::_fsStartTransfer(XferKind kind, const char *id, Volume *v,
                                     const char *path, const char *url, uint64_t size) {
  // Only one transfer at a time — the cloud serialises per device, but guard
  // anyway so a stray second request can't spawn a second task / clobber state.
  bool busy;
  portENTER_CRITICAL(&_xferMux);
  busy = _xfer.active;
  portEXIT_CRITICAL(&_xferMux);
  if (busy) { _fsReplyErr(id, "EBUSY"); return; }

  if (!_fsPathOk(path) || !path[1]) { _fsReplyErr(id, "EINVAL"); return; }
  if (!url || strncmp(url, "https://", 8) != 0) { _fsReplyErr(id, "EINVAL"); return; }
  if (WiFi.status() != WL_CONNECTED) { _fsReplyErr(id, "EOFFLINE"); return; }

  // For a pull the file must exist now (the task reports its real size as total).
  if (kind == XFER_PULL && !v->fs->exists(path)) { _fsReplyErr(id, "ENOENT"); return; }

  // Populate the shared struct, then spawn the task. Strings are copied so the
  // task owns its own data once running.
  _xfer.kind = kind;
  _xfer.vol  = v;
  _xfer.path = String(path);
  _xfer.url  = String(url);
  strncpy(_xfer.id, id, sizeof(_xfer.id) - 1); _xfer.id[sizeof(_xfer.id) - 1] = '\0';
  _xfer.err[0]    = '\0';
  _xfer.doneBytes = 0;
  _xfer.totalBytes = size;            // push knows the size up front; pull fills it in
  _xfer.ok        = false;
  _xfer.done      = false;
  _xfer.progress  = false;
  _xfer.active    = true;

  // Core 0 (Arduino loop runs on core 1) keeps the HTTP/file IO off loop().
  // 8 KB stack covers HTTPClient + TLS comfortably; priority 1 (below loop).
  BaseType_t r = xTaskCreatePinnedToCore(_xferTaskTrampoline, "fg_xfer", 8192,
                                         this, 1, &_xfer.task, 0);
  if (r != pdPASS) {
    portENTER_CRITICAL(&_xferMux);
    _xfer.active = false;
    portEXIT_CRITICAL(&_xferMux);
    _xfer.task = nullptr;
    _fsReplyErr(id, "EIO");
    FG_LOG("files: failed to start transfer task");
    return;
  }
  FG_LOG("files: %s started — %s on \"%s\"", kind == XFER_PULL ? "pull" : "push",
         _xfer.path.c_str(), v->id);
}

void FluxgridClass::_xferTaskTrampoline(void *arg) {
  static_cast<FluxgridClass *>(arg)->_xferRun();
  vTaskDelete(nullptr);   // task ends here; handle is cleared by the main thread
}

// The transfer task body. Does ONLY HTTP + file IO. Never touches MQTT. Writes
// outcome/progress into _xfer under _xferMux; run() publishes them.
void FluxgridClass::_xferRun() {
  XferKind kind = _xfer.kind;
  Volume  *v    = _xfer.vol;
  String   path = _xfer.path;
  String   url  = _xfer.url;
  bool     ok   = false;
  const char *err = "EIO";

  WiFiClientSecure client;
  _fsPrepSecure(client);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(20000);            // per-operation socket timeout

  if (kind == XFER_PULL) {
    // DOWNLOAD: stream the file's bytes out via HTTP PUT to the presigned URL.
    fs::File f = v->fs->open(path, FILE_READ);
    if (!f || f.isDirectory()) {
      if (f) f.close();
      err = "ENOENT";
    } else {
      uint64_t total = f.size();
      portENTER_CRITICAL(&_xferMux);
      _xfer.totalBytes = total;
      portEXIT_CRITICAL(&_xferMux);

      if (!http.begin(client, url)) {
        err = "EIO";
      } else {
        http.addHeader("Content-Type", "application/octet-stream");
        // Stream from a counting wrapper so progress advances live as HTTPClient
        // pulls chunks from the file (bounded RAM — no full-file buffer). It
        // updates doneBytes + the progress flag under the mux on every chunk.
        _FgCountingFile counter(f, &_xfer.doneBytes, &_xfer.progress, &_xferMux);
        int code = http.sendRequest("PUT", &counter, (size_t)total);
        if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED || code == HTTP_CODE_NO_CONTENT) {
          ok = true;
        } else {
          err = "EIO";
          FG_LOG("files: pull PUT http=%d", code);
        }
        http.end();
      }
      f.close();
    }
  } else {
    // UPLOAD: HTTP GET the presigned URL and stream the body into the file.
    fs::File f = v->fs->open(path, FILE_WRITE);   // truncates / creates (overwrite)
    if (!f) {
      err = "EIO";
    } else if (!http.begin(client, url)) {
      err = "EIO";
      f.close();
    } else {
      int code = http.GET();
      if (code != HTTP_CODE_OK && code != HTTP_CODE_PARTIAL_CONTENT) {
        FG_LOG("files: push GET http=%d", code);
        err = "EIO";
        http.end();
        f.close();
      } else {
        int len = http.getSize();    // -1 if the server didn't send Content-Length
        if (len > 0) {
          portENTER_CRITICAL(&_xferMux);
          _xfer.totalBytes = (uint64_t)len;
          portEXIT_CRITICAL(&_xferMux);
        }
        Stream *stream = http.getStreamPtr();   // Stream* — version-agnostic
        uint8_t buf[1024];           // bounded chunk buffer
        bool writeErr = false;
        uint64_t got = 0;
        // Read while the socket is connected or has buffered bytes left.
        while (http.connected() && (len < 0 || got < (uint64_t)len)) {
          size_t avail = stream->available();
          if (avail) {
            int toRead = (int)(avail > sizeof(buf) ? sizeof(buf) : avail);
            int n = stream->readBytes(buf, toRead);
            if (n > 0) {
              if (f.write(buf, n) != (size_t)n) { writeErr = true; break; }
              got += n;
              portENTER_CRITICAL(&_xferMux);
              _xfer.doneBytes = got;
              _xfer.progress  = true;
              portEXIT_CRITICAL(&_xferMux);
            }
          } else {
            delay(1);                // nothing buffered yet — yield, don't spin
          }
        }
        f.flush();
        f.close();
        http.end();
        if (writeErr)            err = "ENOSPC";          // write failed → almost always full
        else if (len < 0)        ok = true;               // no length advertised: trust EOF
        else                     ok = (got >= (uint64_t)len);
        if (!ok && !writeErr)    err = "EIO";
      }
    }
  }

  // Record the outcome; run() sees `done` and publishes the final message.
  portENTER_CRITICAL(&_xferMux);
  _xfer.ok = ok;
  if (!ok) { strncpy(_xfer.err, err, sizeof(_xfer.err) - 1); _xfer.err[sizeof(_xfer.err) - 1] = '\0'; }
  _xfer.done = true;
  portEXIT_CRITICAL(&_xferMux);
}

// Called every run() from the MAIN thread. Publishes any pending progress sample
// and, once the task is done, the final response — then frees the task slot.
void FluxgridClass::_fsPollTransfer() {
  bool active, done, wantProgress, ok;
  uint64_t doneB, totalB;
  XferKind kind;
  char id[40];
  char err[12];

  portENTER_CRITICAL(&_xferMux);
  active       = _xfer.active;
  done         = _xfer.done;
  wantProgress = _xfer.progress;
  ok           = _xfer.ok;
  doneB        = _xfer.doneBytes;
  totalB       = _xfer.totalBytes;
  kind         = _xfer.kind;
  if (active) { _xfer.progress = false; }
  // Snapshot id/err while holding the lock.
  strncpy(id, _xfer.id, sizeof(id)); id[sizeof(id) - 1] = '\0';
  strncpy(err, _xfer.err, sizeof(err)); err[sizeof(err) - 1] = '\0';
  portEXIT_CRITICAL(&_xferMux);

  if (!active) return;

  // Periodic progress (throttled to ~3/s so we don't flood the broker).
  static unsigned long lastProg = 0;
  if (wantProgress && !done && (millis() - lastProg >= 300)) {
    lastProg = millis();
    String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
    s += "\",\"ev\":\"progress\",\"done\":"; s += String((double)doneB, 0);
    s += ",\"total\":"; s += String((double)totalB, 0); s += "}";
    _fsPublish(s);
  }

  if (!done) return;

  // Final message.
  if (ok) {
    // pull echoes the byte count as "size"; push just acknowledges.
    String s = "{\"id\":\""; _fgJsonEscape(String(id), s);
    s += "\",\"ok\":true";
    if (kind == XFER_PULL) { s += ",\"size\":"; s += String((double)doneB, 0); }
    s += "}";
    _fsPublish(s);
    FG_LOG("files: transfer %s done (%lu bytes)", id, (unsigned long)doneB);
  } else {
    _fsReplyErr(id, err);
  }

  // Free the slot. The task already called vTaskDelete(nullptr) on its way out.
  portENTER_CRITICAL(&_xferMux);
  _xfer.active = false;
  _xfer.done   = false;
  _xfer.task   = nullptr;
  _xfer.vol    = nullptr;
  _xfer.kind   = XFER_NONE;
  portEXIT_CRITICAL(&_xferMux);
  _xfer.path = String();
  _xfer.url  = String();
}
