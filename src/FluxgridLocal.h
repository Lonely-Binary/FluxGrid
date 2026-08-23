/*
  FluxgridLocal — optional, header-only companion that serves a Fluxgrid
  dashboard from the ESP32 itself (its own AP or your LAN), with no cloud.

  It is a SEPARATE object from Fluxgrid on purpose: the heavy web-server
  dependencies live only in sketches that #include this header, so cloud-only
  sketches stay lean (Arduino compiles the core library once, separately — it
  can't see a per-sketch #define, but it never sees these includes either).

      #include <Fluxgrid.h>
      #include <FluxgridLocal.h>     // pulls in ESPAsyncWebServer + LittleFS

      void setup() {
        Fluxgrid.manageWiFi(false);
        WiFi.softAP("Fluxgrid-Device", "fluxgrid123");   // or AP+STA, or LAN STA
        Fluxgrid.cloud(false);        // optional: pure local, no broker
        Fluxgrid.begin();
        FluxgridLocal.startWeb();     // http://192.168.4.1/
      }
      void loop() {
        Fluxgrid.run();
        FluxgridLocal.loop();         // pumps the captive-portal DNS
        Fluxgrid.write("temp", readTemp());   // tees to the local dashboard live
      }

  Data bridge (automatic, via Fluxgrid's hooks):
    • Fluxgrid.write(handle, v)  → pushed to browsers over SSE (/stream)
    • POST /write {pin,value}    → Fluxgrid.inject() → fires your onReceive()

  Requires: ESPAsyncWebServer, AsyncTCP, ArduinoJson, LittleFS (ESP32 core).
  MIT License · Lonely Binary
*/
#ifndef FLUXGRID_LOCAL_H
#define FLUXGRID_LOCAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include "Fluxgrid.h"

#ifndef FLUXGRID_LOCAL_MAX_PINS
#define FLUXGRID_LOCAL_MAX_PINS 32
#endif

// Minimal onboarding page, served until a dashboard is pushed. Self-contained,
// polls /status and reloads into the player once a dashboard arrives.
static const char FLUXGRID_SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fluxgrid · Setup</title><style>body{background:#0c0f14;color:#e6eaf2;font:14px/1.5 system-ui,sans-serif;
display:grid;place-items:center;min-height:100vh;margin:0}.c{max-width:420px;padding:24px;border:1px solid #232a36;
border-radius:14px;background:#141923}h1{font-size:17px;margin:0 0 6px}p{color:#8a93a6}b{color:#2dd4bf}</style></head>
<body><div class="c"><h1>◆ Fluxgrid device</h1><p>This device has no dashboard yet. Open the Fluxgrid app, choose
<b>Push to device</b> on this network, and this page will load your dashboard automatically.</p>
<p id="s">…</p></div><script>async function p(){try{let s=await(await fetch('/status',{cache:'no-store'})).json();
document.getElementById('s').textContent=(s.hasDashboard&&s.hasPlayer)?'Ready — loading…':'Waiting for a dashboard…';
if(s.hasDashboard&&s.hasPlayer)location.reload();}catch(e){}}p();setInterval(p,3000);</script></body></html>)HTML";

class FluxgridLocalClass {
public:
  // Start the local web server. Call after Fluxgrid.begin() and after WiFi
  // (AP / STA / AP+STA) is up. `captivePortal` redirects unknown hosts to the
  // dashboard so joining the AP pops it open (only meaningful in AP mode).
  void startWeb(bool captivePortal = true) {
    if (_started) return;
    if (!LittleFS.begin(true)) { FG_LOG("FluxgridLocal: LittleFS mount failed"); }
    _captive = captivePortal;

    // Data bridge OUT: every Fluxgrid.write() updates the cache + pushes SSE.
    Fluxgrid.onWrite([this](const char *handle, const String &value) {
      _cache(handle, value);
      _sendValue(handle, value);
    });

    _routes();
    _events.onConnect([this](AsyncEventSourceClient *c) { c->send(_hydrate().c_str(), NULL, millis()); });
    _server.addHandler(&_events);
    _server.begin();

    if (_captive && _apActive()) _dns.start(53, "*", WiFi.softAPIP());
    _started = true;
    FG_LOG("FluxgridLocal: web server up — %s", url().c_str());
  }

  void stopWeb() {
    if (!_started) return;
    _dns.stop();
    _server.end();
    _started = false;
    FG_LOG("FluxgridLocal: web server stopped");
  }

  // Pump the captive-portal DNS. Call every loop() when the web server is up.
  void loop() { if (_started && _captive && _apActive()) _dns.processNextRequest(); }

  bool running() const { return _started; }

  // Best URL to reach the dashboard: the AP IP if an AP is up, else the LAN IP.
  String url() const {
    IPAddress ip = _apActive() ? WiFi.softAPIP() : WiFi.localIP();
    return String("http://") + ip.toString() + "/";
  }

private:
  AsyncWebServer  _server{80};
  AsyncEventSource _events{"/stream"};
  DNSServer       _dns;
  bool            _started = false;
  bool            _captive = true;

  struct Entry { char pin[24]; String value; bool used = false; };
  Entry _entries[FLUXGRID_LOCAL_MAX_PINS];

  static bool _apActive() {
    wifi_mode_t m = WiFi.getMode();
    return m == WIFI_AP || m == WIFI_AP_STA;
  }

  void _cache(const char *pin, const String &value) {
    for (auto &e : _entries)
      if (e.used && strncmp(e.pin, pin, sizeof(e.pin)) == 0) { e.value = value; return; }
    for (auto &e : _entries)
      if (!e.used) { strncpy(e.pin, pin, sizeof(e.pin) - 1); e.pin[sizeof(e.pin) - 1] = '\0'; e.value = value; e.used = true; return; }
  }

  // SSE "value" frame for one stream (matches the player's contract).
  void _sendValue(const char *pin, const String &value) {
    if (!_started) return;
    JsonDocument doc;
    doc["type"] = "value";
    doc["pin"]  = pin;
    doc["value"] = value.toFloat();
    doc["raw"]   = value;
    doc["ts"]    = (uint32_t)millis();
    String out; serializeJson(doc, out);
    _events.send(out.c_str(), NULL, millis());   // NULL event → browser EventSource.onmessage
  }

  // Full snapshot sent to a freshly-connected browser.
  String _hydrate() {
    JsonDocument doc;
    doc["type"] = "hydrate";
    JsonObject streams = doc["streams"].to<JsonObject>();
    for (auto &e : _entries) {
      if (!e.used) continue;
      JsonObject s = streams[e.pin].to<JsonObject>();
      s["value"] = e.value.toFloat();
      s["raw"]   = e.value;
      s["history"].to<JsonArray>();
    }
    String out; serializeJson(doc, out); return out;
  }

  static bool _hasPlayer()    { return LittleFS.exists("/player.html") || LittleFS.exists("/player.html.gz"); }
  static bool _hasDashboard() { return LittleFS.exists("/dashboard.json"); }

  void _serveFile(AsyncWebServerRequest *req, const char *path, const char *type) {
    String gz = String(path) + ".gz";
    if (LittleFS.exists(gz)) {
      AsyncWebServerResponse *r = req->beginResponse(LittleFS, gz, type);
      r->addHeader("Content-Encoding", "gzip");
      r->addHeader("Cache-Control", "public, max-age=900");
      req->send(r);
    } else if (LittleFS.exists(path)) {
      req->send(LittleFS, path, type);
    } else {
      _serveSetup(req);
    }
  }

  void _serveSetup(AsyncWebServerRequest *req) {
    AsyncWebServerResponse *r = req->beginResponse_P(200, "text/html", (const uint8_t *)FLUXGRID_SETUP_HTML, strlen_P(FLUXGRID_SETUP_HTML));
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  }

  void _routes() {
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
      if (_hasPlayer() && _hasDashboard()) _serveFile(req, "/player.html", "text/html");
      else _serveSetup(req);
    });
    _server.on("/dashboard.json", HTTP_GET, [this](AsyncWebServerRequest *req) {
      _serveFile(req, "/dashboard.json", "application/json");
    });
    _server.on("/version", HTTP_GET, [](AsyncWebServerRequest *req) {
      if (LittleFS.exists("/version.json")) req->send(LittleFS, "/version.json", "application/json");
      else req->send(200, "application/json", "{\"player\":null}");
    });
    _server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument doc;
      doc["hasPlayer"]    = _hasPlayer();
      doc["hasDashboard"] = _hasDashboard();
      doc["version"]      = FLUXGRID_VERSION;
      doc["ssid"]         = WiFi.softAPSSID();
      doc["ip"]           = (_apActive() ? WiFi.softAPIP() : WiFi.localIP()).toString();
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    // Control sink — route through Fluxgrid.inject() so it behaves exactly like
    // a cloud control write (updates read() + fires onReceive).
    _server.addHandler(new AsyncCallbackJsonWebHandler("/write", [](AsyncWebServerRequest *req, JsonVariant json) {
      JsonObject o = json.as<JsonObject>();
      if (o["pin"].is<const char *>()) {
        String pin = o["pin"].as<String>();
        String val = o["value"].is<bool>() ? String(o["value"].as<bool>() ? 1 : 0) : o["value"].as<String>();
        Fluxgrid.inject(pin.c_str(), val);
      }
      req->send(204);
    }));

    // "Push to device": stream an uploaded file onto LittleFS.
    _server.on("/upload", HTTP_POST,
      [](AsyncWebServerRequest *req) { req->send(204); }, nullptr,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static File f;
        if (index == 0) {
          String name = req->hasParam("name") ? req->getParam("name")->value() : "dashboard.json";
          name.replace("/", ""); name.replace("..", "");
          f = LittleFS.open("/" + name, "w");
        }
        if (f) f.write(data, len);
        if (f && index + len == total) f.close();
      });

    // Captive portal: send unknown hosts/paths to the dashboard.
    _server.onNotFound([this](AsyncWebServerRequest *req) {
      if (_captive && _apActive()) req->redirect(url());
      else req->send(404, "text/plain", "not found");
    });
  }
};

#if __cplusplus >= 201703L
inline FluxgridLocalClass FluxgridLocal;
#else
static FluxgridLocalClass FluxgridLocal;
#endif

#endif // FLUXGRID_LOCAL_H
