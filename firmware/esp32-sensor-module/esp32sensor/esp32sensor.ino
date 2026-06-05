#include "config.h"
#include <DHT.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
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

// NTP settings
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = -3 * 3600;  // UTC-3 (Brazil)
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

// ---------- declarations ----------
void connectToWiFi();
void connectToMQTT();
void mqttCallback(char *topic, byte *payload, unsigned int length);
String getTimestamp();

// ----------------------------------
void setup() {
  pinMode(RLED, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  lcd.begin(16, 2);
  lcd.print("R$ ");

  Serial.begin(115200);
  dht.begin();

  connectToWiFi();

  esp_client.setInsecure();
  mqtt_client.setBufferSize(1024);
  mqtt_client.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt_client.setKeepAlive(60);
  mqtt_client.setCallback(mqttCallback);
  connectToMQTT();
}

void connectToWiFi() {
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

  device_id  = WiFi.macAddress();
  base_topic = "devices/" + device_id + "/";

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
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "no-time";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y/%m/%d/%Hh/%Mmin/%Ss", &timeinfo);
  return String(buf);
}

void connectToMQTT() {
  while (!mqtt_client.connected()) {
    String client_id = "esp32-client-" + String(WiFi.macAddress());
    Serial.printf("Connecting to MQTT Broker as %s...\n", client_id.c_str());
    if (mqtt_client.connect(client_id.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.println("Connected to MQTT broker");
      mqtt_client.subscribe((base_topic + "commands").c_str());
      mqtt_client.subscribe("general");

      String msg = "<<<<<<<<< ESP32 (" + device_id + ") ONLINE >>>>>>>>";
      mqtt_client.publish("general", msg.c_str());
    } else {
      digitalWrite(LED_BUILTIN, HIGH);
      Serial.print("Failed to connect to MQTT broker, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" Retrying in 5 seconds.");
      delay(2500);
      digitalWrite(LED_BUILTIN, LOW);
      delay(2500);
    }
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
    if (doc.containsKey("price")) {
      price = doc["price"];
      lcd.setCursor(2, 0);
      char buf[10];
      dtostrf(price, 1, 2, buf);
      lcd.print(buf);
      lcd.print("     ");
    }
    if (doc.containsKey("flag"))             flag             = doc["flag"];
    if (doc.containsKey("samplingInterval")) samplingInterval = doc["samplingInterval"];
    if (doc.containsKey("showsensordata"))   showsensordata   = doc["showsensordata"];
  }
}

void loop() {
  if (!mqtt_client.connected()) {
    connectToMQTT();
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
    String msg = "{\"time\":\"" + timestamp + "\",\"temperature\":" + String(temperature) +
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
