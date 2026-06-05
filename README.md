# FreshTrack

An IoT system that monitors the freshness, temperature, and humidity of products in real time, using paired ESP32 sensor and camera devices. The system automatically analyzes product condition and suggests optimized pricing based on freshness data.

---

## What it does

Each monitored product shelf is represented by a **group**, which consists of:

- An **ESP32 sensor device** that reads temperature and humidity and displays the current price on an LCD screen
- An **ESP32 camera device** that periodically captures images of the product and uploads them to cloud storage

Captured images are then processed by a **computer vision pipeline** that estimates freshness (unripe / ripe / overripe ratios). All data flows through **MQTT** into a **backend pipeline** that writes to a **Supabase** database. A **dashboard** lets store owners monitor their groups and manage devices across one or more stores.

---

## Project structure

```
dashboard/       # (WIP) Web dashboard for store owners
docs/            # (WIP) Additional documentation
firmware/
  esp32sensor/   # Arduino sketch for the sensor device
  esp32cam/      # Arduino sketch for the camera device
  config.h       # WiFi, MQTT, and Supabase credentials (not committed)
mqtt_pipeline/   # (WIP) Backend that consumes MQTT messages and writes to Supabase
supabase/
  db/            # setup.sql — full database + storage schema
.gitignore
```

---

## Setup

### 1. Supabase

1. Create a project at [supabase.com](https://supabase.com)
2. Go to **SQL Editor** and run `supabase/db/setup.sql` — this creates all tables, foreign keys, indexes, the `captured_images` storage bucket, and the upload policy in one shot
3. Copy your **project URL** and **anon key** from **Project Settings → API**

### 2. HiveMQ (MQTT broker)

1. Create a free cluster at [hivemq.com](https://www.hivemq.com)
2. Create a set of credentials (username + password)
3. Note your cluster URL and port (8883 for TLS)

### 3. Firmware credentials

1. Copy `firmware/config.h.example` to `firmware/config.h`
2. Fill in your credentials:

```cpp
#define WIFI_SSID         "your_wifi"
#define WIFI_PASSWORD     "your_wifi_password"

#define MQTT_BROKER       "your.hivemq.cloud"
#define MQTT_USERNAME     "your_mqtt_username"
#define MQTT_PASSWORD     "your_mqtt_password"
#define MQTT_PORT         8883

#define SUPABASE_URL      "https://yourproject.supabase.co"
#define SUPABASE_ANON_KEY "your_anon_key"
```

> `config.h` is gitignored and never committed.

### 4. Flashing the sensor device (ESP32)

1. Open `firmware/esp32sensor/esp32sensor.ino` in Arduino IDE
2. Install dependencies via Library Manager:
   - `Adafruit DHT sensor library`
   - `Adafruit Unified Sensor`
   - `PubSubClient`
   - `ArduinoJson`
   - `LiquidCrystal`
3. Select board: **ESP32 Dev Module**
4. Flash

### 5. Flashing the camera device (ESP32-CAM)

1. Open `firmware/esp32cam/esp32cam.ino` in Arduino IDE
2. Install dependencies via Library Manager:
   - `PubSubClient`
   - `ArduinoJson`
3. Select board: **AI Thinker ESP32-CAM**
4. Under **Tools → PSRAM → Enabled**
5. Flash

---

## MQTT topics

All topics are scoped to the device MAC address (colons removed), e.g. `246F28AA3B12`.

| Topic | Direction | Description |
|---|---|---|
| `devices/<id>/sensordata` | device → broker | Temperature, humidity readings |
| `devices/<id>/captured_images` | device → broker | Image upload notification |
| `devices/<id>/commands` | broker → device | Config updates (see below) |
| `general` | both | Online/offline announcements |

### Command message format

Send any combination of these fields to `devices/<id>/commands`:

```json
{
  "samplingInterval": 5000,
  "price": 12.50,
  "flag": true,
  "showsensordata": true
}
```

---

## Device behavior

- Settings (`samplingInterval`, `price`, `flag`, etc.) are persisted to the device's non-volatile storage and survive power cycles
- The sensor LCD shows current price and optionally live temperature/humidity
- The red LED on the sensor device is toggled remotely via the `flag` command
- The camera device captures a UXGA (1600×1200) image at each interval and uploads it to the `captured_images` Supabase storage bucket
