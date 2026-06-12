/*
  Fluxgrid — GenericNode (autoConfig demo)

  Flash this once. Then configure the device entirely from the web dashboard
  — no IDE, no cable. The board subscribes to its retained config topic and
  hot-applies any change within seconds.

  Config schema (set via dashboard → Devices → Configure):
  {
    "version": 1,
    "inputs": [
      { "pin": "V1", "source": "adc",    "gpio": 4, "interval": 2000, "map": [0, 4095, 0, 50] },
      { "source": "sensor", "driver": "dht22",  "gpio": 15, "interval": 5000,
        "pinTemp": "V2", "pinHum": "V3" },
      { "source": "sensor", "driver": "bme280", "gpio": 0,  "addr": 118, "interval": 10000,
        "pinTemp": "V4", "pinHum": "V5", "pinPress": "V6" },
      { "source": "sensor", "driver": "ds18b20","gpio": 16, "interval": 5000,
        "pinTemp": "V7" }
    ],
    "outputs": [{ "pin": "V10", "target": "digital", "gpio": 26 }],
    "pwm":     [{ "pin": "V11", "gpio": 25, "freq": 5000, "res": 8 }]
  }

  Sensor driver optional dependencies (install in Arduino IDE / PlatformIO):
    DHT22/DHT11  → "DHT sensor library" by Adafruit (or DHTesp)
    BME280       → "Adafruit BME280 Library" + "Adafruit Unified Sensor"
    DS18B20      → "DallasTemperature" + "OneWire" by Paul Stoffregen

  Drivers are detected at compile time via __has_include — only installed
  libraries are compiled in. Missing libraries = that driver is silently skipped.

  Capability boundaries (for more, write custom C++):
    ✓ ADC (analogRead)       ✓ Digital read/write
    ✓ LEDC PWM               ✓ Linear map on ADC
    ✓ DHT22/11               ✓ BME280 (I2C)
    ✓ DS18B20 (1-Wire)       ✗ Interrupt-driven inputs
*/
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define FG_TOKEN   "paste-device-token"   // one string per device, from the dashboard
#include <Fluxgrid.h>            // ← credentials must be #defined ABOVE this line

void setup() {
  Serial.begin(115200);
  Fluxgrid.enableOTA("ota-password");  // optional, remove if not needed
  Fluxgrid.autoConfig();               // enable generic firmware mode
  Fluxgrid.begin();                    // server, port and TLS are built in
}

void loop() {
  Fluxgrid.run();  // handles WiFi, MQTT, OTA, config reads, and timed sends
}
