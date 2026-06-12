#include "config.h"
#include <DHT.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

#define DHT11PIN 33
#define DHTTYPE  DHT11
#define RS 13
#define E  12
#define D4 14
#define D5 27
#define D6 26
#define D7 25
#define RLED 32

// NTP settings — sync to real UTC (offset 0) so timestamps stored with "Z"
// suffix are genuinely UTC and the dashboard can compare them to Date.now()
// without any correction.
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 0;       // UTC — no local offset
const int   daylightOffset_sec = 0;

unsigned long lastPublish      = 0;
unsigned long samplingInterval = 2000;

float price          = 0;
bool  flag           = false;
bool  showsensordata = false;

String device_id;
String base_topic;

WiFiClientSecure esp_client;
PubSubClient     mqtt_client(esp_client);
LiquidCrystal    lcd(RS, E, D4, D5, D6, D7);
DHT              dht(DHT11PIN, DHTTYPE);
Preferences      prefs;

// ---------- declarations ----------
void connectToWiFi();
bool connectToMQTT();
void reconnectIfNeeded();
void mqttCallback(char *topic, byte *payload, unsigned int length);
String getTimestamp();
void lcdStatus(String line1, String line2 = "");
void loadConfig();
void saveConfig();

// ----------------------------------
void loadConfig() {
  prefs.begin("config", true);  // read-only
  samplingInterval = prefs.getULong("samplingInterval", 2000);
  price            = prefs.getFloat("price", 0);
  flag             = prefs.getBool("flag", false);
  showsensordata   = prefs.getBool("showsensordata", false);
  prefs.end();
  Serial.println("Config loaded from NVS");
}

void saveConfig() {
  prefs.begin("config", false);  // read-write
  prefs.putULong("samplingInterval", samplingInterval);
  prefs.putFloat("price", price);
  prefs.putBool("flag", flag);
  prefs.putBool("showsensordata", showsensordata);
  prefs.end();
  Serial.println("Config saved to NVS");
}

void lcdStatus(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  if (line2.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2.substring(0, 16));
  }
}

void setup() {
  pinMode(RLED, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  lcd.begin(16, 2);
  Serial.begin(115200);
  dht.begin();

  loadConfig();

  connectToWiFi();

  esp_client.setInsecure();
  mqtt_client.setBufferSize(1024);
  mqtt_client.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt_client.setKeepAlive(60);
  mqtt_client.setCallback(mqttCallback);
  connectToMQTT();

  // Boot complete — show default display
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("R$ ");
}

void connectToWiFi() {
  lcdStatus("Connecting to", "WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    Serial.print(".");
    delay(200);
  }
  Serial.println("\nConnected to WiFi");
  lcdStatus("WiFi connected!", WiFi.localIP().toString());
  delay(1500);

  device_id  = WiFi.macAddress();
  device_id.replace(":", "");
  base_topic = "devices/" + device_id + "/";

  lcdStatus("Syncing time...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    Serial.print(".");
    delay(200);
  }
  Serial.println("\nTime synced!");
  lcdStatus("Time synced!");
  delay(1000);
}

// Returns a genuine UTC timestamp string ending in "Z".
// Because gmtOffset_sec = 0, getLocalTime() returns UTC directly.
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "no-time";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

bool connectToMQTT() {
  String client_id = "esp32-client-" + String(WiFi.macAddress());
  lcdStatus("Connecting to", "MQTT broker...");
  Serial.printf("Connecting to MQTT Broker as %s...\n", client_id.c_str());

  if (mqtt_client.connect(client_id.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("Connected to MQTT broker");
    mqtt_client.subscribe((base_topic + "commands").c_str());
    mqtt_client.subscribe("general");

    String msg = "<<<<<<<<< ESP32 (" + device_id + ") ONLINE >>>>>>>>";
    mqtt_client.publish("general", msg.c_str());

    lcdStatus("MQTT connected!");
    delay(1500);

    // Restore price display
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("R$ ");
    char buf[10];
    dtostrf(price, 1, 2, buf);
    lcd.print(buf);
    return true;
  }

  Serial.print("MQTT failed, rc=");
  Serial.println(mqtt_client.state());
  lcdStatus("MQTT failed!", "rc=" + String(mqtt_client.state()));
  return false;
}

void reconnectIfNeeded() {
  // Step 1: fix WiFi if it dropped
  if (WiFi.status() != WL_CONNECTED) {
    lcdStatus("WiFi lost...", "Reconnecting");
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      delay(200);
      Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\nWiFi reconnect failed, will retry next loop");
      lcdStatus("WiFi failed", "Retrying...");
      delay(3000);
      return;
    }

    Serial.println("\nWiFi reconnected");
  }

  // Step 2: fix MQTT if WiFi is up but MQTT dropped
  if (!mqtt_client.connected()) {
    connectToMQTT();
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);
  Serial.print("Message: ");
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  Serial.println("\n-----------------------");

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  if (String(topic) == (base_topic + "commands")) {
    bool changed = false;
    if (doc.containsKey("price")) {
      price = doc["price"];
      lcd.setCursor(2, 0);
      char buf[10];
      dtostrf(price, 1, 2, buf);
      lcd.print(buf);
      lcd.print("     ");
      changed = true;
    }
    if (doc.containsKey("flag"))             { flag             = doc["flag"];             changed = true; }
    if (doc.containsKey("samplingInterval")) { samplingInterval = doc["samplingInterval"]; changed = true; }
    if (doc.containsKey("showsensordata"))   { showsensordata   = doc["showsensordata"];   changed = true; }
    if (changed) saveConfig();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED || !mqtt_client.connected()) {
    reconnectIfNeeded();
    return;
  }

  mqtt_client.loop();

  if (millis() - lastPublish >= samplingInterval) {
    lastPublish = millis();

    float humidity    = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("Failed to read from DHT sensor!");
      return;
    }

    String timestamp = getTimestamp();
    String msg = "{\"device_id\":\"" + device_id + "\",\"time\":\"" + timestamp + "\",\"temperature\":" + String(temperature) +
                 ",\"humidity\":" + String(humidity) + "}";
    mqtt_client.publish((base_topic + "sensordata").c_str(), msg.c_str());

    digitalWrite(RLED, flag ? HIGH : LOW);

    lcd.setCursor(2, 0);
    char buf[10];
    dtostrf(price, 1, 2, buf);
    lcd.print(buf);
    lcd.print("     ");

    if (showsensordata) {
      lcd.setCursor(0, 1);
      lcd.print("T:");
      lcd.print(temperature, 1);
      lcd.write(223);
      lcd.print("C H:");
      lcd.print(humidity, 0);
      lcd.print("%");
    } else {
      lcd.setCursor(0, 1);
      lcd.print("                ");
    }
  }
}
