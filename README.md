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

Then install Fluxgrid one of two ways:
- **Arduino IDE → Library Manager** — search "Fluxgrid" and install, **or**
- **Sketch → Include Library → Add .ZIP Library…** and pick the downloaded ZIP.

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

See `examples/LedControl/LedControl.ino` for a complete sketch that drives both LED types from dashboard widgets.

## Debug logging

Debug logging is **on by default**. Open **Tools → Serial Monitor** at **115200 baud**; lines are prefixed `[Fluxgrid]`.

To turn debug **off**, `#define FLUXGRID_DEBUG 0` **before** the include:

```cpp
#define FLUXGRID_DEBUG 0
#include <Fluxgrid.h>
```

| Define (before `#include`) | Default | What it does |
|---|---|---|
| `FLUXGRID_DEBUG` | `1` (on) | `0` silences all serial logging (and skips the auto `Serial.begin`). |
| `FLUXGRID_DEBUG_BAUD` | `115200` | Baud rate `begin()` uses when it auto-starts Serial. |

## Security note

Each device has its **own** MQTT account and a broker ACL that locks it to its
own `fluxgrid/<token>/#` topic subtree — one device cannot read or write
another's topics.

TLS is on by default (`secure(true)`). For production devices, pin your broker's root CA:

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

MIT License · Lonely Binary
