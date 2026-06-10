# FreshTrack

An IoT system that monitors the freshness, temperature, and humidity of products in real time, using paired ESP32 sensor and camera devices. The system automatically analyzes product condition and suggests optimized pricing based on freshness data.

---

## What it does

Each monitored product shelf is represented by a **group**, which consists of:

- An **ESP32 sensor device** that reads temperature and humidity and displays the current price on an LCD screen
- An **ESP32 camera device** that periodically captures images of the product and uploads them to cloud storage

Captured images are processed by a **computer vision pipeline** that estimates freshness (unripe / ripe / overripe ratios). All data flows through **MQTT** into a **backend pipeline** that writes to a **Supabase** database. A **dashboard** lets store owners monitor their groups and manage devices across one or more stores.

---

## Architecture

```
ESP32 Sensor  ──┐
                ├──► HiveMQ (MQTT) ──► mqtt_pipeline  ──► Supabase DB
ESP32 CAM    ──┘         │                                     ▲
                         │             computer_vision_model ──┘
                         │             optimized_price_model ──┘
                         │
                    Dashboard (Netlify) ──► mqtt_pipeline /command
```

All three Python services (`mqtt_pipeline`, `computer_vision_model`, `optimized_price_model`) are deployed as separate services on **Railway**. The dashboard is a static HTML file hosted on **Netlify**.

---

## Project structure

```
dashboard/                  # Static HTML dashboard (hosted on Netlify)
firmware/
  esp32-sensor-module/      # Arduino sketch for the sensor device
  esp32-cam-module/         # Arduino sketch for the camera device
mqtt_pipeline/              # Ingests MQTT messages, writes to Supabase, exposes /command endpoint
computer_vision_model/      # Subscribes to image captures, runs CV inference, writes freshness to DB
optimized_price_model/      # Periodically computes recommended prices, publishes to devices
supabase/
  setup.sql                 # Full DB + storage schema
  policies.sql              # Row Level Security policies
  product_info.sql          # Example product seed data
.gitignore
```

---

## Setup

### 1. Supabase (database)

1. Create a project at [supabase.com](https://supabase.com)
2. Go to **SQL Editor** and run `supabase/setup.sql` — this creates all tables, foreign keys, indexes, the `captured_images` storage bucket, and the upload policy
3. Run `supabase/policies.sql` to enable Row Level Security with anon access for all tables
4. Optionally run `supabase/product_info.sql` to seed an example product (Banana)
5. Copy your **Project URL** and **anon key** from **Project Settings → API**

---

### 2. HiveMQ (MQTT broker)

1. Create a free Serverless cluster at [hivemq.com](https://www.hivemq.com)
2. Under **Access Management**, create a set of credentials (username + password)
3. Note your cluster hostname (e.g. `abc123.s1.eu.hivemq.cloud`) and port (`8883` for TLS)

---

### 3. Railway (backend services)

Each of the three Python services is deployed independently on Railway.

#### General steps for each service

1. Create a new project at [railway.app](https://railway.app)
2. Connect your GitHub repository and select the service folder (`mqtt_pipeline`, `computer_vision_model`, or `optimized_price_model`) as the root
3. Set the environment variables listed below for each service
4. Railway will auto-detect Python and install dependencies from `requirements.txt`
5. Add a `Procfile` or set the start command to `python main.py` in Railway's settings if not auto-detected

#### `mqtt_pipeline` environment variables

```env
MQTT_BROKER=your.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=your_mqtt_username
MQTT_PASSWORD=your_mqtt_password
SUPABASE_URL=https://yourproject.supabase.co
SUPABASE_KEY=your_anon_or_service_role_key
DASHBOARD_SECRET=          # optional: set a secret to restrict /command access
PORT=8080
```

After deploying, Railway will give this service a public URL (e.g. `https://mqtt-pipeline-production.up.railway.app`). Note it — the dashboard needs the `/command` endpoint (`https://your-pipeline.up.railway.app/command`).

#### `computer_vision_model` environment variables

```env
MQTT_BROKER=your.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=your_mqtt_username
MQTT_PASSWORD=your_mqtt_password
SUPABASE_URL=https://yourproject.supabase.co
SUPABASE_KEY=your_anon_or_service_role_key
PORT=8080
```

> **Note:** This service downloads the `TCleo/banana-maturity-mobile-vit-small` model from Hugging Face on first boot. Railway's free tier may need a memory upgrade (at least 1 GB recommended for PyTorch inference).

#### `optimized_price_model` environment variables

```env
MQTT_BROKER=your.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=your_mqtt_username
MQTT_PASSWORD=your_mqtt_password
SUPABASE_URL=https://yourproject.supabase.co
SUPABASE_KEY=your_anon_or_service_role_key
RUN_INTERVAL_SECONDS=300   # how often to recompute prices (default: 5 minutes)
PORT=8080
```

---

### 4. Netlify (dashboard)

1. In `dashboard/webpage.html`, fill in the three configuration constants near the top of the `<script>` block:

```js
const SUPABASE_URL      = 'https://yourproject.supabase.co';
const SUPABASE_ANON_KEY = 'your_anon_key';
const COMMAND_ENDPOINT  = 'https://your-mqtt-pipeline.up.railway.app/command';
```

2. Go to [netlify.com](https://www.netlify.com) and create a new site
3. Drag and drop the `dashboard/` folder onto the Netlify deploy area, or connect your GitHub repo and set the publish directory to `dashboard/`
4. Netlify will publish the site immediately — no build step required

---

### 5. Firmware — sensor device (ESP32)

1. Copy `firmware/esp32-sensor-module/esp32sensor/config-example.h` to `config.h` in the same folder
2. Fill in your credentials:

```cpp
#define WIFI_SSID     "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"

#define MQTT_BROKER   "your.hivemq.cloud"
#define MQTT_USERNAME "your_mqtt_username"
#define MQTT_PASSWORD "your_mqtt_password"
#define MQTT_PORT     8883
```

3. Open `esp32sensor.ino` in Arduino IDE and install the following libraries via Library Manager:
   - `Adafruit DHT sensor library`
   - `Adafruit Unified Sensor`
   - `PubSubClient`
   - `ArduinoJson`
   - `LiquidCrystal`
4. Select board: **ESP32 Dev Module**
5. Flash

---

### 6. Firmware — camera device (ESP32-CAM)

1. Copy `firmware/esp32-cam-module/esp32cam/config-example.h` to `config.h` in the same folder
2. Fill in your credentials:

```cpp
#define WIFI_SSID         "your_wifi_name"
#define WIFI_PASSWORD     "your_wifi_password"

#define MQTT_BROKER       "your.hivemq.cloud"
#define MQTT_USERNAME     "your_mqtt_username"
#define MQTT_PASSWORD     "your_mqtt_password"
#define MQTT_PORT         8883

#define SUPABASE_URL      "https://yourproject.supabase.co"
#define SUPABASE_ANON_KEY "your_anon_key"
```

3. Open `esp32cam.ino` in Arduino IDE and install the following libraries via Library Manager:
   - `PubSubClient`
   - `ArduinoJson`
4. Select board: **AI Thinker ESP32-CAM**
5. Under **Tools → PSRAM → Enabled**
6. Flash

---

## First run checklist

After all services are deployed and devices are flashed:

1. Power on both ESP32 devices — they will connect to WiFi, sync NTP time, and announce themselves on the `general` MQTT topic
2. In the dashboard, log in (create a user row directly in Supabase for now), register your devices under **Dispositivos**, and create a **Mesa** linking the sensor and camera to a product
3. The sensor will immediately start publishing temperature and humidity; `mqtt_pipeline` writes these to Supabase and the dashboard reflects them within one auto-refresh cycle (30 s)
4. The camera will capture an image at its `samplingInterval` (default 30 s), upload it to Supabase Storage, and publish a notification; `computer_vision_model` picks this up and writes freshness scores
5. `optimized_price_model` runs every `RUN_INTERVAL_SECONDS`, computes a recommended price, and pushes it to the sensor LCD via MQTT; the dashboard shows the suggested price with an **Aplicar** button to confirm

---

## MQTT topics

All device topics are scoped to the device MAC address (colons removed), e.g. `246F28AA3B12`.

| Topic                           | Direction       | Description                       |
|---------------------------------|-----------------|-----------------------------------|
| `devices/<id>/sensordata`       | device → broker | Temperature and humidity readings |
| `devices/<id>/captured_images`  | device → broker | Image upload notification         |
| `devices/<id>/commands`         | broker → device | Config updates (see below)        |
| `general`                       | both            | Online/offline announcements      |

### Command message formats

Send to `devices/<id>/commands` — include any combination of fields:

**Sensor device:**
```json
{
  "samplingInterval": 5000,
  "price": 12.50,
  "flag": true,
  "showsensordata": true
}
```

**Camera device:**
```json
{
  "samplingInterval": 30000
}
```

---

## Device behavior

- All settings (`samplingInterval`, `price`, `flag`, etc.) are persisted to non-volatile storage and survive power cycles
- The sensor LCD shows the current price and optionally live temperature/humidity
- The red LED on the sensor is toggled remotely via the `flag` command (set automatically when sensor conditions are out of range)
- The camera captures a UXGA (1600×1200) JPEG at each interval and uploads it directly to the `captured_images` Supabase storage bucket