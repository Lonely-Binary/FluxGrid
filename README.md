# Fluxgrid Arduino Library

The simple ESP32 client for the [Fluxgrid](https://fluxgrid.lonelybinary.cn) IoT platform.

Complex inside (WiFi, MQTT, auto-reconnect, Last-Will online/offline, topic
plumbing), **simple outside**. A complete device is about ten lines:

```cpp
#define WIFI_SSID  "wifi"
#define WIFI_PASS  "pass"
#define FG_TOKEN   "DEVICE_TOKEN"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

void setup() {
  Fluxgrid.begin();                  // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();
  Fluxgrid.write("temp", analogRead(4) * (50.0 / 4095.0));      // push a reading
  digitalWrite(2, Fluxgrid.read("relay").asBool());            // read a Switch back
}
```

`"temp"` and `"relay"` are **datastream handles** — the short name each widget
shows on the dashboard. Drop a widget on the canvas and it creates a datastream
for you; use that handle here. Pick anything readable (`temp`, `relay`,
`pump`, `red`); it's what travels in the MQTT topic.

## Install

**Requirements:** ESP32 Arduino core + [`PubSubClient`](https://github.com/knolleary/pubsubclient) (install from Library Manager).

Then install Fluxgrid one of three ways:
- **Arduino IDE → Library Manager** — search "Fluxgrid" and install, **or**
- **Sketch → Include Library → Add .ZIP Library…** and pick the downloaded ZIP, **or**
- copy the `Fluxgrid/` folder into your `Arduino/libraries/` directory.

Restart the IDE; you'll find examples under **File → Examples → Fluxgrid**.

## Get your credentials

In your dashboard: **add a device → "Pair device"**. Each device has **one
token** — copy it into `FG_TOKEN`. That single string bundles the device's
routing token + its own MQTT user/pass (joined as `<token>.<user>.<pass>`); the
library splits it for you, so you only ever paste one value per device. The
pairing screen shows a ready-to-flash sketch with it filled in.

> The three `#define`s **must come before `#include <Fluxgrid.h>`** — the
> library reads them to implement the zero-argument `Fluxgrid.begin()`. (Arduino
> compiles your sketch and the library separately, so a value defined *after*
> the include is invisible to it.)

## API

| Call | What it does |
|---|---|
| `Fluxgrid.begin()` | Connect WiFi + cloud using `WIFI_SSID` / `WIFI_PASS` / `FG_TOKEN`. Call once in `setup()`. |
| `Fluxgrid.begin(ssid, pass, token, user, pass)` | Explicit form, if you'd rather pass the split values than `#define` them. |
| `Fluxgrid.run()` | Keep the link alive + deliver writes. Call every `loop()`. |
| `Fluxgrid.write(handle, value)` | Push telemetry to a datastream (`"temp"`, …). Overloaded: `int` / `long` / `float` / `double` / `bool` / `String`. |
| `Fluxgrid.read(handle)` | Latest value for a datastream, as a `FluxValue` — convert with `.asInt()` / `.asFloat()` / `.asBool()` / `.asString()`. `.isEmpty()` is `true` until the first value arrives. |
| `Fluxgrid.has(handle)` | `true` once any value has been received for the handle. |
| `Fluxgrid.onReceive(handle, fn)` | Fire a callback once per incoming write. The lambda gets a `FluxValue`: `onReceive("relay", [](FluxValue v){ … v.asBool() … })`. |
| `Fluxgrid.onConnected(fn)` | Runs each time the cloud connection is (re)established. |
| `Fluxgrid.connected()` | `true` when linked to the cloud. |

**Options (call before `begin`):** only needed for a self-hosted broker or to
pin the broker's certificate — the official cloud works with no options.

| Call | Default |
|---|---|
| `Fluxgrid.setServer(host, port)` | `mqtt.lonelybinary.cn` : `8883` |
| `Fluxgrid.secure(on)` | `true` (MQTTS/TLS) — call `secure(false)` for a plaintext broker |
| `Fluxgrid.setCACert(pem)` | not set — TLS encrypts but accepts any server cert. Pass your broker's root CA (PEM) to pin it; see [Security note](#security-note). |

## Serial monitor on the dashboard

Want your device's serial output in the **browser**, not just over USB? Drop a
**Terminal** widget on the canvas (Display group) and bind it to a `log`
datastream — then send lines to it one of two ways. Both tee to **both** the USB
Serial Monitor and the cloud, share one line buffer, and publish each completed
line (on `\n`) to the log handle (default `"log"`; change with
`Fluxgrid.setLogHandle("dbg")`). See the **SerialMonitor** example.

**A — zero code change.** `#define FLUXGRID_CAPTURE_SERIAL 1` *before* the
include and keep writing `Serial.print` / `println` / `printf` exactly as you do
now:

```cpp
#define FLUXGRID_CAPTURE_SERIAL 1
#include <Fluxgrid.h>
...
Serial.println("hello");        // → USB Serial Monitor *and* the dashboard
```

**B — explicit.** Leave the macro off and call `Fluxgrid.println(...)` (a full
`Print`: `print` / `println` / `printf`) for just the lines you want online; the
real `Serial` is left completely untouched:

```cpp
Fluxgrid.println("hello");      // → USB + dashboard, Serial unchanged
```

| Call | What it does |
|---|---|
| `FLUXGRID_CAPTURE_SERIAL` (define `1` before include) | Alias `Serial` to the tee so existing `Serial.*` calls also reach the dashboard. Default `0` (off). |
| `Fluxgrid.println(...)` / `.print(...)` / `.printf(...)` | Explicitly tee a line to USB + the log datastream, without redefining `Serial`. |
| `Fluxgrid.setLogHandle("dbg")` | Publish captured lines to a different datastream handle (default `"log"`). |

**Things to know:**

- **`FLUXGRID_CAPTURE_SERIAL` replaces the `Serial` *token*.** It only affects
  `Serial` *after* the include in that `.ino` (`Serial1` / `Serial2` are
  untouched; the library's own internals stay on the real UART). Because it's a
  blunt text substitution, taking `Serial`'s address or binding it to a
  `HardwareSerial&` won't compile, and other libraries `#include`d *after*
  Fluxgrid that use `Serial` in inline/header code get captured too — so put
  `#include <Fluxgrid.h>` **last**, or use route B instead.
- **Lines reach the cloud only once connected.** Early boot output is USB-only
  (it still prints there). Nothing is back-filled on connect.
- **The cloud rate-limits one stream to a few lines/sec.** Very chatty logging
  is throttled on the server (the USB monitor always shows everything).
- **A line that's purely a number** (e.g. `42`) is stored as a numeric value,
  not a log line. Mixed text is fine.

## LED control

The library has built-in support for two kinds of LEDs with a unified API — no external LED library needed.

| Constant | Meaning |
|---|---|
| `LED_NORMAL` | Regular LED wired to a GPIO (default) |
| `LED_WS2812` | Single WS2812 / WS2812B RGB LED, driven via the ESP32 RMT peripheral |

**Register your LEDs once in `setup()`**, before `Fluxgrid.begin()`:

```cpp
Fluxgrid.addLed(26);                             // regular LED on GPIO 26
Fluxgrid.addLed(48, LED_WS2812);                 // WS2812 on GPIO 48 (default: white)
Fluxgrid.addLed(48, LED_WS2812, 255, 80, 0);     // WS2812 with a custom default colour (R, G, B)
```

**Control:**

```cpp
Fluxgrid.ledOn(26);                              // regular LED → HIGH
Fluxgrid.ledOff(26);                             // regular LED → LOW

Fluxgrid.ledOn(48);                              // WS2812 → last stored colour
Fluxgrid.ledOn(48, 0, 200, 255);                 // WS2812 → new colour (also stored as default)
Fluxgrid.ledOff(48);                             // WS2812 → off (0, 0, 0)
```

The WS2812 driver uses the ESP32's **RMT hardware peripheral** directly, so the signal timing is handled in hardware — it stays accurate even when WiFi and MQTT are running. No `noInterrupts()` hacks needed.

See `examples/LedControl/LedControl.ino` for a complete sketch that drives both LED types from dashboard widgets.

## Debug logging

Debug logging is **on by default**. The library narrates what it's doing —
WiFi join, cloud connect, every value sent and received, config applied — to the
serial monitor, so you can see exactly what's happening while you build. Open
**Tools → Serial Monitor** at **115200 baud**; lines are prefixed `[Fluxgrid]`:

```
[Fluxgrid] debug on — starting (token=abc123, host=mqtt.lonelybinary.cn:8883, tls=yes)
[Fluxgrid] WiFi: connecting to "my-wifi" ...
[Fluxgrid] WiFi: connected, IP 192.168.1.42
[Fluxgrid] cloud: connecting to mqtt.lonelybinary.cn:8883 as dev_abc ...
[Fluxgrid] cloud: connected — online, subscribed fluxgrid/abc123/w/+
[Fluxgrid] write: temp = 24.50
[Fluxgrid] recv: relay = 1
```

You don't need to call `Serial.begin()` yourself — when debug is on, `begin()`
starts it for you. To turn debug **off**, `#define FLUXGRID_DEBUG 0` **before**
the include:

```cpp
#define FLUXGRID_DEBUG 0
#include <Fluxgrid.h>
```

| Define (before `#include`) | Default | What it does |
|---|---|---|
| `FLUXGRID_DEBUG` | `1` (on) | `0` silences all serial logging (and skips the auto `Serial.begin`). |
| `FLUXGRID_DEBUG_BAUD` | `115200` | Baud rate `begin()` uses when it auto-starts Serial. |

## WiFi Scan widget

The **WiFi Scan** widget shows the ESP32's `WiFi.scanNetworks()` results as a
live Radar, Channel Spectrum, or List view with three selectable color themes.

**Dashboard setup:** create a datastream named `"wifi_scan"` (kind: `json`),
drop a WiFi Scan widget on the canvas, and bind it to `"wifi_scan"`.

**Sketch:** see **File → Examples → Fluxgrid → WiFiScan**. The sketch publishes
a compact JSON payload every 10 seconds:

```cpp
// payload format (short keys keep it under 512 chars for 8 networks)
// {"n":[{"s":"SSID","r":-62,"c":4,"e":"WPA+WPA2","b":"AA:BB:CC:DD:EE:FF"},…]}
Fluxgrid.write("wifi_scan", payload);   // payload is a String built by the sketch
```

Key field names: `s`=SSID, `r`=RSSI (dBm), `c`=channel, `e`=encryption type,
`b`=BSSID. The BSSID determines where each network sits on the radar — networks
with a real BSSID always appear in the same position across scans.

**Things to know:**
- `WiFi.scanNetworks()` takes 1–4 s per call; keep `SCAN_INTERVAL ≥ 5000 ms`.
- The widget shows a built-in placeholder scan while waiting for the first
  payload so the tile always looks alive.
- The Channel Spectrum shows 2.4 GHz channels 1–13. 5 GHz networks that happen
  to have the same channel number will still render (cosmetically only).

## How it maps to your dashboard

- `Fluxgrid.write("temp", x)` → any **Gauge / Value / Chart** bound to the `temp` datastream updates live.
- A **Switch / Slider / Button** → read it with `Fluxgrid.read("relay")` or an `onReceive("relay", …)` handler on the device.
- The device shows **online/offline** automatically, so widgets grey out when it
  drops. Presence uses the MQTT **Last-Will** for the offline edge (the broker
  publishes `offline` the moment the device disconnects ungracefully) plus a
  retained `online` **heartbeat** the library republishes every 30s. The
  heartbeat keeps devices that send little or no telemetry (e.g. relay-only
  boards) correctly marked online — call `Fluxgrid.run()` regularly in `loop()`
  for it to fire.

Under the hood it speaks the Fluxgrid topic scheme so you don't have to:

```
fluxgrid/<token>/v/<pin>     telemetry up
fluxgrid/<token>/w/<pin>     control writes down
fluxgrid/<token>/status      online/offline (retained)
```

## Security note

Each device has its **own** MQTT account and a broker ACL that locks it to its
own `fluxgrid/<token>/#` topic subtree — one device cannot read or write
another's topics. Those per-device credentials are bundled into the single
`FG_TOKEN` string (`<token>.<user>.<pass>`) and split by the library at
`begin()`.

TLS is on by default (`secure(true)`), so the link is always encrypted. Out of
the box the server certificate is **not verified** — friendly for getting
started, but an active attacker who can redirect your traffic could impersonate
the broker. For production devices, pin your broker's root CA:

```cpp
static const char ROOT_CA[] = R"PEM(
-----BEGIN CERTIFICATE-----
...your broker's root CA certificate...
-----END CERTIFICATE-----
)PEM";

void setup() {
  Fluxgrid.setCACert(ROOT_CA);   // before begin()
  Fluxgrid.begin();
}
```

With a pinned CA the device refuses to connect to anything that can't present a
certificate signed by it. (The PEM string must stay alive — a global, as above,
is the usual way.)

MIT License · Lonely Binary
