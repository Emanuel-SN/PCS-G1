#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "esp_camera.h"

// ESP32-CAM AI-Thinker pin mapping
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// NTP settings — sync to real UTC (offset 0) so timestamps stored with "Z"
// suffix are genuinely UTC and the dashboard can compare them to Date.now()
// without any correction.
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 0;
const int   daylightOffset_sec = 0;

unsigned long lastCapture      = 0;
unsigned long samplingInterval = 30000;

String device_id;
String base_topic;

WiFiClientSecure esp_client;
WiFiClientSecure http_client;
PubSubClient     mqtt_client(esp_client);
Preferences      prefs;

// ---------- declarations ----------
void connectToWiFi();
bool connectToMQTT();
void reconnectIfNeeded();
void mqttCallback(char *topic, byte *payload, unsigned int length);
String getTimestamp();
String getFilenameTimestamp();
bool uploadToSupabase(uint8_t *buf, size_t len, String path);
void captureAndUpload();
void loadConfig();
void saveConfig();

// ----------------------------------
void loadConfig() {
  prefs.begin("config", true);
  samplingInterval = prefs.getULong("samplingInterval", 30000);
  prefs.end();
  Serial.println("Config loaded from NVS");
}

void saveConfig() {
  prefs.begin("config", false);
  prefs.putULong("samplingInterval", samplingInterval);
  prefs.end();
  Serial.println("Config saved to NVS");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Booting...");

  loadConfig();

  // Camera config
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_UXGA;
  config.jpeg_quality = 10;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    while (true) delay(1000);
  }
  Serial.println("Camera ready");

  connectToWiFi();

  // Configure TLS and MQTT — done once at boot only.
  // esp_client is re-stopped/restarted on each MQTT reconnect in
  // reconnectIfNeeded(), but setInsecure/setServer only need to run once.
  esp_client.setInsecure();
  http_client.setInsecure();
  mqtt_client.setBufferSize(512);
  mqtt_client.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt_client.setKeepAlive(60);
  mqtt_client.setCallback(mqttCallback);
  connectToMQTT();

  Serial.println("Boot complete");
}

// ---------------------------------------------------------------------------
// connectToWiFi — called once at boot only.
// Sets device_id, base_topic, and syncs NTP (one-time only; RTC keeps time
// after this so we never need to re-sync on reconnect).
// ---------------------------------------------------------------------------
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi: " + WiFi.localIP().toString());

  device_id  = WiFi.macAddress();
  device_id.replace(":", "");
  base_topic = "devices/" + device_id + "/";

  // Sync NTP once — the ESP32 RTC keeps time across reconnects.
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing NTP time");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nTime synced!");
}

// ---------------------------------------------------------------------------
// reconnectIfNeeded — called every loop() iteration.
//
// Step 1: if WiFi is down, attempt reconnect with a 15 s timeout and bail
//         out of this cycle if it doesn't come back. We do NOT touch MQTT
//         while WiFi is down — that's what caused the broker flooding.
//
// Step 2: only once WiFi is confirmed up, attempt a single MQTT reconnect
//         if needed. We explicitly stop() the TLS socket before each attempt
//         so the broker never sees a half-open zombie connection.
// ---------------------------------------------------------------------------
void reconnectIfNeeded() {
  // ── Step 1: WiFi ──────────────────────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\nWiFi reconnect failed, will retry next loop");
      delay(3000);
      return;   // bail — do NOT attempt MQTT while WiFi is still down
    }

    Serial.println("\nWiFi reconnected");
  }

  // ── Step 2: MQTT (WiFi is confirmed up at this point) ────────────────────
  if (!mqtt_client.connected()) {
    connectToMQTT();
  }
}

// ---------------------------------------------------------------------------
// connectToMQTT — single attempt per call (no inner while loop).
// Always stop()s the TLS socket first to close any zombie session on the
// broker side before opening a fresh one.
// ---------------------------------------------------------------------------
bool connectToMQTT() {
  // Tear down any leftover TLS session before opening a new one.
  // Without this the broker accumulates half-open connections.
  esp_client.stop();
  delay(100);

  String client_id = "esp32cam-" + device_id;
  Serial.printf("Connecting to MQTT as %s...\n", client_id.c_str());

  if (mqtt_client.connect(client_id.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("Connected to MQTT broker");
    mqtt_client.subscribe((base_topic + "commands").c_str());
    mqtt_client.subscribe("general");

    String msg = "<<<<<<<<< ESP32-CAM (" + device_id + ") ONLINE >>>>>>>>";
    mqtt_client.publish("general", msg.c_str());
    return true;
  }

  Serial.printf("MQTT failed, rc=%d — will retry next loop\n", mqtt_client.state());
  delay(5000);   // back off before the next loop() iteration tries again
  return false;
}

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "no-time";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

String getFilenameTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "no-time";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &timeinfo);
  return String(buf);
}

// ---------------------------------------------------------------------------
// MQTT callback
// ---------------------------------------------------------------------------
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println("Message: " + message);
  Serial.println("-----------------------");

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  if (String(topic) == (base_topic + "commands")) {
    bool changed = false;
    if (doc.containsKey("samplingInterval")) { samplingInterval = doc["samplingInterval"]; changed = true; }
    if (changed) saveConfig();
  }
}

// ---------------------------------------------------------------------------
// Supabase image upload
// ---------------------------------------------------------------------------
bool uploadToSupabase(uint8_t *buf, size_t len, String storagePath) {
  http_client.stop();

  String host = String(SUPABASE_URL);
  host.replace("https://", "");

  String url = "/storage/v1/object/captured_images/" + storagePath;
  Serial.println("Uploading to Supabase: " + url);

  if (!http_client.connect(host.c_str(), 443)) {
    Serial.println("Supabase connection failed");
    return false;
  }

  http_client.println("POST " + url + " HTTP/1.1");
  http_client.println("Host: " + host);
  http_client.println("Authorization: Bearer " + String(SUPABASE_ANON_KEY));
  http_client.println("Content-Type: image/jpeg");
  http_client.println("Content-Length: " + String(len));
  http_client.println("Connection: close");
  http_client.println();
  http_client.write(buf, len);

  String response = "";
  unsigned long timeout = millis();
  while (http_client.connected() && millis() - timeout < 10000) {
    if (http_client.available()) {
      response += (char)http_client.read();
    }
  }
  http_client.stop();

  Serial.println("Supabase response: " + response.substring(0, 200));
  return response.indexOf("200") > 0 || response.indexOf("201") > 0;
}

// ---------------------------------------------------------------------------
// captureAndUpload — retries until success, but yields to reconnectIfNeeded
// between attempts so WiFi/MQTT issues are handled cleanly.
// ---------------------------------------------------------------------------
void captureAndUpload() {
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  bool ok = false;
  while (!ok) {
    // Keep connections healthy during retry loop
    reconnectIfNeeded();
    if (!mqtt_client.connected()) {
      // No point uploading if we can't publish the result — wait and retry
      delay(5000);
      continue;
    }
    mqtt_client.loop();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed, retrying in 5s...");
      delay(5000);
      continue;
    }

    String timestamp     = getTimestamp();
    String fileTimestamp = getFilenameTimestamp();
    String storagePath   = device_id + "/" + fileTimestamp + ".jpg";

    ok = uploadToSupabase(fb->buf, fb->len, storagePath);
    esp_camera_fb_return(fb);

    if (!ok) {
      Serial.println("Upload failed, retrying in 5s...");
      delay(5000);
      continue;
    }

    StaticJsonDocument<300> doc;
    doc["device_id"]      = device_id;
    doc["time"]           = timestamp;
    doc["storage_bucket"] = "captured_images";
    doc["storage_path"]   = storagePath;

    char mqttMsg[300];
    serializeJson(doc, mqttMsg);
    mqtt_client.publish((base_topic + "captured_images").c_str(), mqttMsg);
    Serial.println("Published: " + String(mqttMsg));
  }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void loop() {
  reconnectIfNeeded();

  if (!mqtt_client.connected()) {
    // reconnectIfNeeded already attempted — skip this cycle
    return;
  }

  mqtt_client.loop();

  if (millis() - lastCapture >= samplingInterval) {
    captureAndUpload();
    lastCapture = millis();
  }
}
