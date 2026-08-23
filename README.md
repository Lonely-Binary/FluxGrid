# Fluxgrid Arduino Library

The simple ESP32 client for the [Fluxgrid](https://fluxgrid.lonelybinary.com) IoT platform.

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

The bundled examples keep each handle in one place as a `#define` ending in
`_HANDLE`, then use that macro everywhere instead of repeating the string — so
matching the code to your widget is a one-line edit:

```cpp
#define RELAY_HANDLE "relay"   // must match your Switch widget's datastream handle
// ...
Fluxgrid.onReceive(RELAY_HANDLE, [](FluxValue v){ digitalWrite(2, v.asBool()); });
```

Rename the widget's handle on the dashboard (or change the `#define`) so the two
agree — the Sample Code window in the editor does this for you when you pick a
**Datastream** from its toolbar.

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
| `Fluxgrid.write(handle, value)` | Push telemetry to a datastream (`"temp"`, …). Overloaded: `int` / `long` / `unsigned int` / `unsigned long` / `float` / `double` / `bool` / `String`. The unsigned overloads let you pass `ESP.getFreeHeap()` and other `uint32_t` values directly. |
| `Fluxgrid.read(handle)` | Latest value for a datastream, as a `FluxValue` — convert with `.asInt()` / `.asFloat()` / `.asBool()` / `.asString()`. `.isEmpty()` is `true` until the first value arrives. |
| `Fluxgrid.has(handle)` | `true` once any value has been received for the handle. |
| `Fluxgrid.onReceive(handle, fn)` | Fire a callback once per incoming write. The lambda gets a `FluxValue`: `onReceive("relay", [](FluxValue v){ … v.asBool() … })`. |
| `Fluxgrid.onConnected(fn)` | Runs each time the cloud connection is (re)established. |
| `Fluxgrid.reportMemory(intervalMs = 5000)` | Auto-publish heap (+ PSRAM) every `intervalMs` in one message for the [Memory Chart widget](#memory-chart-widget) — call once in `setup()`. `0` turns it off. |
| `Fluxgrid.connected()` | `true` when linked to the cloud. |
| `Fluxgrid.writeWiFiScan(handle = "wifi_scan", maxNets = 16)` | Scan nearby WiFi and publish it (sorted, JSON) to a [WiFi Scan widget](#wifi-scan-widget). Blocks 1–4 s; call on a timer. Returns the count. |
| `Fluxgrid.startWiFiScan()` / `Fluxgrid.pollWiFiScan(handle, maxNets)` | Non-blocking WiFi scan — `start` once, `poll` every `loop()` (`-1` running, `-2` none, `≥0` done & published). See [WiFi Scan widget](#wifi-scan-widget). |

**Options (call before `begin`):** only needed for a self-hosted broker or to
pin the broker's certificate — the official cloud works with no options.

### Choosing a region

Fluxgrid runs one cloud per region, each with its own broker, accounts and
devices. A device belongs to exactly **one** of them — the region you signed up
in. Its credentials do not exist on the other, so a device pointed at the wrong
region will fail to authenticate.

| Region | Broker | Dashboard |
|---|---|---|
| Global (default) | `mqtt.lonelybinary.com` | https://fluxgrid.lonelybinary.com |
| China | `mqtt.lonelybinary.cn` | https://fluxgrid.lonelybinary.cn |

The global cloud is the default, so most sketches need no region line at all.
On the China cloud, add one call before `begin()`:

```cpp
void setup() {
  Fluxgrid.region("cn");
  Fluxgrid.begin();
}
```

The sketch the dashboard generates already contains the right line for the
region you are signed in to, so if you copy from **Pair device** you can ignore
this entirely.

| Call | Default |
|---|---|
| `Fluxgrid.region(code)` | `"com"` — the global cloud. Use `"cn"` for the China cloud. |
| `Fluxgrid.setServer(host, port)` | `mqtt.lonelybinary.com` : `8883` |
| `Fluxgrid.secure(on)` | `true` (MQTTS/TLS) — call `secure(false)` for a plaintext broker |
| `Fluxgrid.setCACert(pem)` | not set — TLS encrypts but accepts any server cert. Pass your broker's root CA (PEM) to pin it; see [Security note](#security-note). |

## Managing WiFi yourself

By default Fluxgrid owns WiFi: `begin()` joins `WIFI_SSID` / `WIFI_PASS` in STA
mode and reconnects for you — the ten-line quickstart. If you'd rather control
the radio (to provision with **WiFiManager** / **Improv**, run an **access
point**, or share the connection with your own code), take it over:

| Call | What it does |
|---|---|
| `Fluxgrid.manageWiFi(false)` | **Call before `begin()`.** The library never touches the radio — you bring up STA / AP / AP+STA yourself and it rides on top. `run()` only observes the link (no mode changes, no reconnect kicks, never blocks). With this off, `begin()` needs only `FG_TOKEN` — no WiFi credentials. |
| `Fluxgrid.cloud(false)` | **Call before `begin()`.** Skip the MQTT broker entirely — a pure local device (e.g. an offline AP). Default on. |
| `Fluxgrid.startAP(ssid, pass = nullptr)` | Bring up the device's own access point (AP+STA, so an existing STA link and the cloud keep working). Pass no password for an open AP. Safe to call after `begin()`. |
| `Fluxgrid.stopAP()` | Tear the access point back down. |

```cpp
#define FG_TOKEN "DEVICE_TOKEN"        // no WIFI_SSID / WIFI_PASS needed
#include <Fluxgrid.h>
#include <WiFi.h>

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.begin("ssid", "pass");          // or WiFiManager / Improv here
  while (WiFi.status() != WL_CONNECTED) delay(250);

  Fluxgrid.manageWiFi(false);          // hands off the radio
  Fluxgrid.begin();                    // cloud rides on your connection
}
void loop() { Fluxgrid.run(); }
```

See the **ByoWiFi** example (File ▸ Examples ▸ Fluxgrid). On the ESP32 the AP
and STA share one radio, so both end up on the same channel — a STA reconnect
can briefly bump connected AP clients.

> Default behaviour is unchanged: existing sketches that `#define WIFI_*` and
> call `begin()` keep working exactly as before — these calls are all opt-in.

## Local dashboard (offline / AP) — `FluxgridLocal`

Serve a dashboard from the ESP32 itself — its own access point or your LAN —
with **no cloud**. This lives in a separate header you opt into with an
`#include`, so cloud-only sketches never pull in the web-server dependencies:

```cpp
#include <Fluxgrid.h>
#include <FluxgridLocal.h>     // adds ESPAsyncWebServer + LittleFS — only here
#include <WiFi.h>

void setup() {
  Fluxgrid.manageWiFi(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Fluxgrid-Device", "fluxgrid123");
  Fluxgrid.cloud(false);        // optional: pure local, no broker
  Fluxgrid.begin();
  FluxgridLocal.startWeb();     // → http://192.168.4.1/
}
void loop() {
  Fluxgrid.run();
  FluxgridLocal.loop();         // pumps the captive-portal DNS
  Fluxgrid.write("temp", readTemp());   // shows live on the local dashboard
}
```

The data bridge is automatic: `Fluxgrid.write()` is pushed to browsers over SSE,
and a control widget tapped in the browser calls your `Fluxgrid.onReceive()` —
the same callback cloud control writes use. Until a dashboard is pushed
(from the Fluxgrid app's **Push to device** — see below), the device serves a
small onboarding page instead of a 404.

| Call | What it does |
|---|---|
| `FluxgridLocal.startWeb(captivePortal = true)` | Start the local web server (port 80). In AP mode also runs a captive portal. Call after `Fluxgrid.begin()` and after WiFi is up. |
| `FluxgridLocal.stopWeb()` | Stop the web server. |
| `FluxgridLocal.loop()` | Call every `loop()` — pumps the captive-portal DNS. |
| `FluxgridLocal.url()` | The best URL to reach it (AP IP, else LAN IP). |

**Requires** (install via Library Manager): `ESPAsyncWebServer`, `AsyncTCP`,
`ArduinoJson`, and a Partition Scheme with a LittleFS partition. See the
**LocalDashboard** example. Note the local AP has no TLS — gate the device
before exposing it on an untrusted network.

### Push a dashboard from the cloud editor

The editor's **Manage ▸ Push to device…** sends the current page's layout to the
device *over the cloud*: the app stages the (filtered) `dashboard.json` and the
device pulls it onto LittleFS `/dashboard.json`, which `FluxgridLocal` then
serves offline. Online-only widgets (maps, fleet, camera, file explorer…) are
dropped automatically; the modal previews exactly what ships.

For the device to **receive** a push it must, at push time:

- be **online and cloud-connected** — `Fluxgrid.cloud(true)` (the default) over
  real WiFi; an **AP+STA** setup receives the push *and* keeps serving offline
  afterwards, and
- expose flash to the file transport —
  `Fluxgrid.addVolume("flash", LittleFS, FG_FLASH)` + `Fluxgrid.enableFiles()`.

The **LocalDashboard** example is pure-offline (`cloud(false)`, AP only) and so
**cannot** receive a cloud push. Use the **LocalDashboardCloud** example (AP+STA,
with `addVolume`/`enableFiles`) when you want to push from the editor. The player
bundle (`player.html.gz`) is flashed/uploaded separately; a routine push only
re-sends the small layout JSON.

## Serial monitor on the dashboard

Want your device's serial output in the **browser**, not just over USB? Drop a
**Terminal** widget on the canvas (Display group) and bind it to a `log`
datastream — then send lines to it one of two ways. Both tee to **both** the USB
Serial Monitor and the cloud, share one line buffer, and publish each completed
line (on `\n`) to the log handle (default `"log"`; change with
`Fluxgrid.setLogHandle("dbg")`). See the **SerialMonitor/CaptureSerial**
(Method A) and **SerialMonitor/ExplicitPrint** (Method B) examples.

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

## File explorer

Expose the device's mounted filesystems to the dashboard's **File Explorer**
widget — browse folders, download and upload files, rename, delete, make
directories and format — straight from the browser.

The split: small **metadata** commands (browse, rename, …) ride MQTT, while file
**bytes** never touch MQTT — they stream over HTTPS to/from short-lived
presigned storage URLs, on a background task, so a multi-megabyte transfer never
stalls `loop()`.

> **You mount the hardware; the library never touches the SD bus or pins.** Bring
> the filesystem up yourself — `LittleFS.begin()`, `SD_MMC.begin()`, `SD.begin(cs)`
> — so you keep full control of partitions, the SD mode (SDMMC vs SPI) and which
> pins it uses. Then register each mounted filesystem and switch the feature on.

```cpp
#include <Fluxgrid.h>
#include <LittleFS.h>
#include <SD_MMC.h>          // or <SD.h> for an SPI card

void setup() {
  LittleFS.begin(true);                          // you mount it
  Fluxgrid.addVolume("flash", LittleFS, FG_FLASH); // capacity auto-detected

  SD_MMC.begin();                                // you mount it
  Fluxgrid.addVolume("sd", SD_MMC, FG_SD);

  Fluxgrid.enableFiles();                        // turn the explorer on
  Fluxgrid.begin();
}

void loop() { Fluxgrid.run(); }
```

On the dashboard, drop a **File Explorer** widget and point it at this device.
Browsing, rename, delete, mkdir and format work out of the box; **download and
upload** additionally need object storage (MinIO) configured for your workspace.

### `addVolume()` — two styles

**Known types (zero extra args).** Pass the global filesystem object and its type
constant; the library reads capacity for you (the `fs::FS` base class can't report
it, so this resolves against the concrete class in *your* sketch):

```cpp
Fluxgrid.addVolume("flash", LittleFS, FG_FLASH);   // on-chip flash
Fluxgrid.addVolume("sd",    SD_MMC,  FG_SD);        // SD over SDMMC
Fluxgrid.addVolume("sd",    SD,      FG_SD);        // …or SD over SPI
Fluxgrid.addVolume("fat",   FFat,    FG_FLASH);     // FATFS on flash
// optional 4th/5th args: a human label and a read-only flag
Fluxgrid.addVolume("flash", LittleFS, FG_FLASH, "Internal Flash", /*readonly=*/true);
```

**Any filesystem (escape hatch).** For a filesystem the known-type path doesn't
recognise, register it as `FG_FS_OTHER` and supply the capacity getters yourself:

```cpp
Fluxgrid.addVolume("data", myFs, FG_FS_OTHER,
                   []() -> uint64_t { return myFs.totalBytes(); },
                   []() -> uint64_t { return myFs.usedBytes();  },
                   "Data", /*readonly=*/false);    // label + readonly optional
```

| Call | What it does |
|---|---|
| `Fluxgrid.addVolume(id, fs, type)` | Register a **known** filesystem (`LittleFS` / `SD` / `SD_MMC` / `FFat`); capacity auto-detected. `type` is `FG_FLASH`, `FG_SD` or `FG_FS_OTHER`. Optional `label`, `readonly`. |
| `Fluxgrid.addVolume(id, fs, type, totalFn, usedFn)` | Register **any** `fs::FS`, supplying capacity getters. Optional `label`, `readonly`. |
| `Fluxgrid.enableFiles()` | Turn the file-explorer handler on. Call once in `setup()`. Until you call it the device never subscribes to the command channel, so sketches that don't use files pay nothing. |

- **`id`** is a short slug you choose (`[A-Za-z0-9_-]`, ≤ 32 chars) — how the
  widget addresses the volume. **`label`** is the human name on the card (defaults
  to `id`). A **`readonly`** volume refuses every write (upload, rename, delete,
  mkdir, format) with `EACCES`.
- Up to **4** volumes by default — raise `FLUXGRID_MAX_VOLUMES` (define it before
  `#include <Fluxgrid.h>`) if a board mounts more.

### LittleFS vs FATFS (flash), and the SD card

- **On-chip flash** — use **LittleFS** for most projects: it's power-loss
  resilient and the default for the ESP32. **FATFS (FFat)** is the alternative
  when you need a FAT layout (e.g. files a PC must read over USB MSC). Either
  way, pick a **Partition Scheme** with a filesystem partition (Tools ▸ Partition
  Scheme) and register it with `FG_FLASH`. The dashboard's **Format** button
  calls the real `.format()`.
- **SD card** — register it with `FG_SD`. There is **no portable `format()` for
  SD**, so on an SD volume the **Format button wipes everything at the root**
  (a full delete) rather than re-creating the filesystem. You initialise the card
  (SDMMC or SPI, with your pins) before `addVolume()`.

See `examples/FileExplorer/FileExplorer.ino` for a complete LittleFS (on-chip
flash) sketch, and `examples/FileExplorerSD/FileExplorerSD.ino` for an SD card
(SDMMC or SPI).

## Debug logging

Debug logging is **on by default**. The library narrates what it's doing —
WiFi join, cloud connect, every value sent and received, config applied — to the
serial monitor, so you can see exactly what's happening while you build. Open
**Tools → Serial Monitor** at **115200 baud**; lines are prefixed `[Fluxgrid]`:

```
[Fluxgrid] debug on — starting (token=abc123, host=mqtt.lonelybinary.com:8883, tls=yes)
[Fluxgrid] WiFi: connecting to "my-wifi" ...
[Fluxgrid] WiFi: connected, IP 192.168.1.42
[Fluxgrid] cloud: connecting to mqtt.lonelybinary.com:8883 as dev_abc ...
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

**Sketch:** the library builds and publishes the payload for you — one call:

```cpp
Fluxgrid.writeWiFiScan();          // scan + sort + publish to "wifi_scan"
```

`writeWiFiScan(handle = "wifi_scan", maxNets = 16)` scans, sorts the networks
strongest-first, serializes them, and publishes — returning the count. It
**blocks for 1–4 s** while the radio scans, so call it on a timer (every ~10 s),
not every `loop()`. See **File → Examples → Fluxgrid → WiFiScan → WiFiScanLoop**.

Need the connection to stay responsive during the scan? Use the non-blocking
pair (**WiFiScanAsync** example):

```cpp
Fluxgrid.startWiFiScan();          // kick the radio, returns immediately
int c = Fluxgrid.pollWiFiScan();   // every loop(): -1 running, -2 none, ≥0 published
```

The published payload is a JSON array, strongest first:

```jsonc
[{"ssid":"NET","rssi":-62,"channel":4,"enc":"WPA2","bssid":"AA:BB:CC:DD:EE:FF"},…]
```

`bssid` determines where each network sits on the radar — networks with a real
BSSID always appear in the same position across scans. The payload is trimmed to
`maxNets` and to the MQTT buffer so it always fits.

**Things to know:**
- The scan takes 1–4 s; use a scan interval of ~10 s (the examples do).
- The widget shows a built-in placeholder scan while waiting for the first
  payload so the tile always looks alive.
- The Channel Spectrum shows 2.4 GHz channels 1–13. 5 GHz networks that happen
  to have the same channel number will still render (cosmetically only).

## WiFi RSSI widget

The **WiFi RSSI** widget shows the device's *own* link strength — how good its
connection to the router is — as a live signal monitor (arc fan, signal bars, or
serial console view, with a rolling history graph and quality %).

Unlike the WiFi Scan widget (which lists *every* nearby network), this reports a
single number: `WiFi.RSSI()` in dBm, for the access point the ESP32 is joined
to. The widget derives the quality %, the strong/weak tier and the history graph
from that one value.

**Dashboard setup:** create a datastream named `"rssi"` (kind: `number`), drop a
WiFi RSSI widget on the canvas, and bind it to `"rssi"`.

**Sketch:** see **File → Examples → Fluxgrid → RssiMonitor**. Publish the link
strength every few seconds:

```cpp
if (WiFi.status() == WL_CONNECTED)     // RSSI is only valid while associated
  Fluxgrid.write("rssi", WiFi.RSSI()); // dBm, e.g. -62
```

Key field: `WiFi.RSSI()` returns dBm where **closer to zero is stronger**
(`≥ -57` strong, `-70` moderate, `-82` weak, below that may drop).

**Things to know:**
- RSSI jumps ±5 dBm sample-to-sample; the example smooths it with an
  exponential moving average so the gauge reads steadily.
- `WiFi.RSSI()` is meaningful only while connected — guard on
  `WiFi.status() == WL_CONNECTED` so a reconnect doesn't publish a bogus `0`.
- One reading is all the widget needs; it computes quality %, tiers, average and
  the history graph itself.
- The **network name (SSID)** shown on the widget is filled in automatically — the
  library reports it to the dashboard once on every cloud connect (and reconnect)
  as part of the [device-info report](#device-info-widget), so there's nothing to
  configure or publish yourself.

## Memory widget

The **Memory** widget tracks the ESP32's heap so you can watch a long-running
device's RAM trend — spot a slow leak, or confirm a refactor freed memory. It
reads **three** datastreams, the three numbers the core exposes:

| Datastream | Value | Meaning |
|---|---|---|
| `free_heap` | `ESP.getFreeHeap()` | bytes free right now |
| `min_free_heap` | `ESP.getMinFreeHeap()` | low-water mark since boot (the worst dip) |
| `max_alloc` | `ESP.getMaxAllocHeap()` | largest single block still allocatable |

**Dashboard setup:** create three datastreams (kind: `number`) named
`"free_heap"`, `"min_free_heap"`, `"max_alloc"`, drop a Memory widget on the
canvas, and bind its three slots to them.

**Sketch:** publish all three every couple of seconds:

```cpp
Fluxgrid.write("free_heap",     ESP.getFreeHeap());
Fluxgrid.write("min_free_heap", ESP.getMinFreeHeap());
Fluxgrid.write("max_alloc",     ESP.getMaxAllocHeap());
```

`free_heap` trending down while `max_alloc` shrinks faster is the classic
fingerprint of heap **fragmentation** — the same total is free, but no longer in
one block. See **File → Examples → Fluxgrid → MemoryMonitor**.

## Memory Chart widget

The **Memory Chart** widget is a richer, four-style take on the Memory widget: it
plots **both** the internal heap **and** external SPI **PSRAM** — as an area
time-series, dual capacity bars, a CRT bar histogram, or animated liquid tanks
(pick the look in the widget's *Style* property).

**One line turns it on.** Call `Fluxgrid.reportMemory()` once in `setup()` and the
library auto-publishes the device's memory every few seconds — no per-loop
`write()` calls, and **no datastreams to create or bind**. All the numbers ride in
a **single MQTT message**, and the widget just picks the device (exactly like the
[Device Info widget](#device-info-widget)):

```cpp
void setup() {
  Fluxgrid.reportMemory();     // heap + PSRAM, every 5 s
  Fluxgrid.begin();
}

void loop() {
  Fluxgrid.run();
}
```

What it publishes each tick (heap always; the PSRAM pair only on boards that have
PSRAM). The heap/PSRAM **totals** are read automatically from the device (the same
`device_info` the Device Info widget reports), so nothing else is sent:

| Value | Source | Meaning |
|---|---|---|
| free heap | `ESP.getFreeHeap()` | heap bytes free right now |
| min free heap | `ESP.getMinFreeHeap()` | heap low-water mark since boot |
| max alloc | `ESP.getMaxAllocHeap()` | largest single block still allocatable (fragmentation) |
| free PSRAM | `ESP.getFreePsram()` | PSRAM bytes free right now |
| min free PSRAM | `ESP.getMinFreePsram()` | PSRAM low-water mark since boot |

**Dashboard setup:** drop a Memory Chart widget on the canvas, and in its
*Properties* panel **pick the device** — no datastreams to wire up. A board with
no PSRAM just leaves that half hidden automatically.

`reportMemory(intervalMs)` takes an optional cadence: the default is 5 s — pass
`2000` for a snappier demo, `30000`+ for a long-term leak watch, or `0` to turn it
back off.

See **File → Examples → Fluxgrid → MemoryChart**.

## Device Info widget

The **Device Info** widget shows the hard facts about a board — chip model,
silicon revision, core count, MAC addresses, flash / PSRAM size, SDK version, why
it last reset, and its live network details. **You don't publish any of this** —
the library gathers it and reports it **automatically on every cloud connect**
(and reconnect), so the widget just works once you drop it on the canvas and
point it at the device.

It travels on the same retained `meta` topic that already carries the WiFi SSID
(so the [WiFi RSSI widget](#wifi-rssi-widget) keeps showing the network name).
The payload is a JSON object — the SSID at the top level plus an `info` block:

```jsonc
{
  "ssid": "MyWiFi",
  "info": {
    "chip": { "model":"ESP32-S3", "rev":"v0.2", "cores":2, "mhz":240,
              "id":"ECDA3B1A2B3C", "features":["WiFi","BLE"] },
    "mem":  { "heap":327680, "flash":4194304, "psram":8388608 },
    "mac":  { "sta":"EC:DA:3B:1A:2B:3C", "ap":"…", "bt":"…" },
    "fw":   { "sdk":"v5.3.1", "reset":"POWERON" },
    "net":  { "ip":"192.168.1.42", "mask":"255.255.255.0", "gw":"192.168.1.1",
              "dns":["192.168.1.1","8.8.8.8"], "rssi":-54, "ch":6,
              "bssid":"AA:BB:CC:DD:EE:FF" }
  }
}
```

The static facts (chip, mem, MACs, SDK) don't change between boots; the `net`
block and `reset` reason reflect the current connection. Fields that a given
chip can't report (e.g. `psram` on a board with none → `0`) are still present so
the widget's layout stays stable.

## Map widget

The **Map** widget plots a position on a [Leaflet](https://leafletjs.com/) map
([CARTO](https://carto.com/) basemap tiles, OpenStreetMap data — no API key
needed). It reads a single datastream; publish to it in either format:

```
"lat,lng"                                  e.g.  "37.7749,-122.4194"
{"lat":37.7749,"lng":-122.4194}            JSON  (note: lng, not lon)
```

The JSON form takes two optional extras:

- **`acc`** — accuracy radius in **metres**. The widget draws a halo of that size
  instead of implying a pinpoint — ideal for coarse fixes (e.g. IP geolocation).
  Omit it for a precise GPS pin.
- **`label`** — text shown in the marker popup / coordinate line (e.g. a city).

```cpp
// Coarse position with a 25 km accuracy halo and a popup label:
Fluxgrid.write("map", "{\"lat\":24.06,\"lng\":120.55,\"acc\":25000,\"label\":\"Taichung\"}");
```

Each new position is appended to a track (the polyline) so movement draws a path.

Two ready-to-flash examples live under **File → Examples → Fluxgrid → Map**:

- **IpLocation** — no hardware; locates the device from its public IP (via
  ip-api.com) and reports it with an honest accuracy circle.
- **Gps** — reads a real GPS module (e.g. u-blox NEO-6M / NEO-M8N) over UART on
  an ESP32-S3 and plots a precise live fix. Needs the **TinyGPS++** library
  (by Mikal Hart) from the Library Manager.

## Control widgets

Control widgets send values *down* to the device. Drop one on the canvas, note
the datastream handle it creates, and read it with `Fluxgrid.read("handle")` or
an `onReceive("handle", …)` callback. Each has a ready-to-flash example under
**File → Examples → Fluxgrid**.

| Widget | Sends | Read it as | Example |
|---|---|---|---|
| **Switch** | `1` / `0` on toggle (latching) | `.asBool()` | Switch → SwitchRelay |
| **Icon Tile** | `1` / `0` on tap (latching) | `.asBool()` | IconTile → IconTileLamp |
| **Button** | `1` while held, `0` on release (momentary) | `.asBool()` | Button → ButtonOnReceive |
| **Slider** | a number between the datastream's min / max | `.asInt()` / `.asFloat()` | Slider → SliderDimmer |
| **Stepper** | the new absolute value (−/+ by the step) | `.asInt()` / `.asFloat()` | Stepper → StepperSetpoint |
| **Segmented** | the selected option **index** (0, 1, 2, …) | `.asInt()` | Segmented → SegmentedMode |
| **RGB** | red / green / blue (0–255) on **three** datastreams | `.asInt()` per channel | RGB → RgbStrip |
| **Switch Group** | `1` / `0` per row, each on its own datastream | `.asBool()` per handle | SwitchGroup → SwitchGroupRoom |
| **Piano** | a MIDI note on press · a recorded take on SEND (**two** datastreams) | see **[Piano widget](#piano-widget)** | Piano → PianoBuzzer |
| **Jukebox** | a whole song on play (**two** datastreams) | see **[Jukebox widget](#jukebox-widget)** | Jukebox → JukeboxBuzzer |

**Notes:**

- A **Switch** and an **Icon Tile** both latch — they stay on/off until tapped
  again; a **Button** is momentary (only "on" while held). Pick by feel.
- The **Segmented** widget sends the option *index*, not its label — option 0 is
  the first entry. Set the labels in the widget's Properties (`Off,Low,High`).
- The **RGB** widget edits one colour but writes its three channels separately,
  so you receive one message per channel (see RgbStrip). Want the colour in a
  **single** message instead? Use the **Color** widget — it packs the colour into
  one 24-bit integer (`0xRRGGBB`) on one datastream, which you unpack on the
  device:

  ```cpp
  int rgb = Fluxgrid.read("color").asInt();
  int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  ```

- The **Switch Group** is several relays grouped into one card; handle each row's
  datastream as you would a single Switch. The card's master toggle flips every
  row, so your per-row handlers cover it for free. Rename rows and set per-row
  icons in the widget's Properties.

## Piano widget

The **Piano** is a playable keyboard that drives a **passive buzzer**. Unlike the
other controls it binds **two** datastreams, so the example reacts to both:

| Datastream | Kind | Carries | When |
|---|---|---|---|
| `note` | Number | the live MIDI note (0–127, `0` = silence) | the instant a key is pressed/released |
| `seq` | Text | the recorded take as one `mseq.v1` JSON string | once, when you tap **SEND** |

**Live play.** Each key press writes its MIDI note to `note`; play it on a passive
buzzer with `tone()` (a passive buzzer needs an AC waveform, so plain
`digitalWrite()` won't do). The buzzer is monophonic, so the widget applies
last-note priority — the newest held note wins, and `0` silences it:

```cpp
static unsigned int midiToFreq(int m) { return lroundf(440.0f * powf(2.0f, (m - 69) / 12.0f)); }

Fluxgrid.onReceive("note", [](FluxValue v) {
  int m = v.asInt();
  if (m > 0) tone(BUZZER, midiToFreq(m));   // 60 = C4, 69 = A4 = 440 Hz
  else       noTone(BUZZER);
});
```

**Record → Send.** Tap **REC** (a 3-2-1 count-in, then up to 30 s), play, then
**SEND**. The whole take — notes *and* timing — arrives on `seq` as one JSON
message:

```json
{ "pin": "seq", "fmt": "mseq.v1", "dur": 4200, "count": 12,
  "events": [[60,1,0],[60,0,180],[62,1,210],[62,0,400]] }
```

`events` is `[midiNote, on(1)/off(0), msOffsetFromStart]`. Parse it with
ArduinoJson and schedule each event by its offset so the buzzer replays the
performance — non-blocking, so `Fluxgrid.run()` keeps the link alive. See the
full **Piano → PianoBuzzer** example for the scheduler.

> **Keep takes short.** The recording is sent as one MQTT message, so the payload
> must fit the MQTT buffer (2 KB by default — a busy 30 s can exceed it). Raise it
> with `#define FLUXGRID_MQTT_BUFFER 1024*8` before the include if you need more.
> A local synth in the browser previews the notes; the real sound is the buzzer.

## Jukebox widget

The **Jukebox** plays whole songs on a passive buzzer — a retro song box whose
library is the wonderful [robsoncouto/arduino-songs](https://github.com/robsoncouto/arduino-songs)
project (**credit: Robson Couto**). Like the Piano it binds **two** datastreams:

| Datastream | Kind | Carries | When |
|---|---|---|---|
| `song` | Text | the whole tune as one `jbq.v1` JSON string | when you press **play** |
| `play` | Boolean | `1` play / `0` stop | on the transport buttons |

The `song` payload is the tune's notes — notes + tempo, nothing else:

```json
{ "fmt": "jbq.v1", "tempo": 120, "m": [60,8, 62,8, 64,4, 0,8, 67,-4] }
```

`m` is a flat `[midi, divider, …]` list: `midi` is the note (0 = rest), `divider`
the length (4 = quarter, 8 = eighth, negative = dotted ×1.5) — the same
convention as the Arduino song sketches. Note length in ms is
`(60000*4 / tempo) / divider`. Convert each note to a `tone()` and schedule it
non-blocking; see the **Jukebox → JukeboxBuzzer** example for the player.

> **Songs are big — raise the MQTT buffer.** A whole song streams in one message,
> and the longest ones serialize to ~3–4 KB, past the default 2 KB. The example
> sets **`#define FLUXGRID_MQTT_BUFFER 1024*8`** (8 KB) *before* the include so
> every song fits. See [Bigger payloads](#bigger-payloads-fluxgrid_mqtt_buffer).

## Bigger payloads (`FLUXGRID_MQTT_BUFFER`)

The library keeps one MQTT buffer that must hold the **whole packet** (topic +
framing + payload) — both for what you publish and what you receive. It defaults
to **2048 bytes**, which covers telemetry, control writes and a ~16-network WiFi
scan. When a widget sends or receives something bigger (the Jukebox streams an
entire song), raise it **before** the include:

```cpp
#define FLUXGRID_MQTT_BUFFER 1024*8   // 8 KB
#include <Fluxgrid.h>
```

It costs that many bytes of RAM (negligible on an ESP32) and is applied for you
in `begin()`.

## Display widgets — the Card

Display widgets go the other way: the device `write()`s a value *up* and the
widget renders it. The firmware is the same one line you already know —
`Fluxgrid.write("handle", value)` — so what's worth knowing is how to dress the
value up on the dashboard.

The **Card** is the most configurable of these: a tinted icon next to a big
reading, with an optional label and a prefix/postfix around the value. A few
things specific to it:

- **It can show text, not just numbers.** Bind a Card to a `kind: text`
  datastream and `write()` a string (`"OK"`, `"FAULT"`) — most widgets can't do
  this. On a text Card the conditional colour compares with *contains / equals*
  instead of `>` / `<`.
- **Unit size** controls the prefix/postfix: *Small* (default) keeps `%` / `$`
  as a compact affix; *Normal* matches the value's size for units like `°C` that
  read as part of the number.
- **Conditional icon colour** recolours the icon when the value meets a
  condition (e.g. `> 30` → red).
- **Templates** — an admin can save a Card's whole look under a name
  (Temperature, Humidity, …) so anyone can apply it to a new Card in one click.

Three ready-to-flash examples under **File → Examples → Fluxgrid → Card**:

| Example | Datastream | Shows |
|---|---|---|
| **TemperatureCard** | number | postfix `°C`, Unit size **Normal**, colour when too hot |
| **BatteryCard** | number | postfix `%`, Unit size **Small** (the default affix) |
| **StatusCard** | text | a status word (`OK` / `WARN` / `FAULT`), colour on `FAULT` |

## How it maps to your dashboard

- `Fluxgrid.write("temp", x)` → any **Gauge / Value / Chart / Card** bound to the `temp` datastream updates live.
- A **Switch / Slider / Button** → read it with `Fluxgrid.read("relay")` or an `onReceive("relay", …)` handler on the device.
- The device shows **online/offline** automatically, so widgets grey out when it
  drops. Presence uses the MQTT **Last-Will** for the offline edge (the broker
  publishes `offline` the moment the device disconnects ungracefully) plus a
  retained `online` **heartbeat** the library republishes every 30s. The
  heartbeat keeps devices that send little or no telemetry (e.g. relay-only
  boards) correctly marked online — call `Fluxgrid.run()` regularly in `loop()`
  for it to fire.
- The library **reports its version** to the dashboard: the presence payload is
  `online <version>` (e.g. `online 0.9.3`), so the device list can show which
  firmware each board runs and flag out-of-date ones. The same version is
  printed to the Serial Monitor on boot (`[Fluxgrid] Fluxgrid library v0.9.3`).
- The library also reports a **device-info** snapshot once per connect on a
  dedicated retained `meta` topic — the WiFi network name (SSID) plus chip,
  memory, MAC, firmware and live-network facts. The WiFi RSSI widget reads the
  SSID from it, and the [Device Info widget](#device-info-widget) shows the rest,
  all without any extra code.

Under the hood it speaks the Fluxgrid topic scheme so you don't have to:

```
fluxgrid/<token>/v/<pin>     telemetry up
fluxgrid/<token>/w/<pin>     control writes down
fluxgrid/<token>/wg/<group>  grouped control write — one JSON packet, e.g. {"x":1,"y":-1}
fluxgrid/<token>/status      online[ <version>] / offline (retained)
fluxgrid/<token>/meta        device info {"ssid":…,"info":{chip,mem,mac,fw,net}} (retained, on connect)
fluxgrid/<token>/fs/req      file-explorer commands down (when enableFiles())
fluxgrid/<token>/fs/res      file-explorer results up    (when enableFiles())
```

Multi-variable control widgets (joystick, RGB picker, range slider) send all of
their values in **one** `wg/<group>` packet so they arrive together (no torn
half-updates) instead of one message per value. The library unpacks it for you
and delivers each field to its own handle — so you still just
`read("x")` / `read("y")` (or `onReceive`), exactly as for any other value.

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
