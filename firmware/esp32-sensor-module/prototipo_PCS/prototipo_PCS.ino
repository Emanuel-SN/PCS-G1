#include <dht11.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#define DHT11PIN 33
#define RS 13
#define E 12
#define D4 14
#define D5 27
#define D6 26
#define D7 25
#define rled 32

String device_id;

// NTP settings
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;  // adjust for your timezone (Brazil = UTC-3)
const int daylightOffset_sec = 0;

// WiFi
const char *ssid = "EMANUEL"; // Enter your WiFi name
const char *password = "Emanuel0805@";  // Enter WiFi password

// MQTT Broker
const char *mqtt_broker = "57df5cda86e746b580ee6ec6dd77babf.s1.eu.hivemq.cloud";
const char *mqtt_username = "esp32";
const char *mqtt_password = "Senha1234";
const int mqtt_port = 8883;

unsigned long lastPublish = 0;
unsigned long samplingInterval = 2000;

float price=10;
float T_min=0;
float T_max=30;
float H_min=50;
float H_max=80;
bool flag=false;
bool showsensordata=false;


// MQTT Topic
String base_topic;

// WiFi and MQTT client initialization
WiFiClientSecure esp_client;
PubSubClient mqtt_client(esp_client);

// Function Declarations
void connectToWiFi();

void connectToMQTT();

void mqttCallback(char *topic, byte *payload, unsigned int length);

LiquidCrystal lcd(RS, E, D4, D5, D6, D7);

dht11 DHT11;

void setup() {

  pinMode(rled, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  lcd.begin(16,2);
  lcd.print("R$ ");

  Serial.begin(115200);
  connectToWiFi();

  esp_client.setInsecure();

  mqtt_client.setBufferSize(1024);
  mqtt_client.setServer(mqtt_broker, mqtt_port);
  mqtt_client.setKeepAlive(60);
  mqtt_client.setCallback(mqttCallback);
  connectToMQTT();
}

void connectToWiFi() {
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      Serial.print(".");
      delay(200);
    }
    Serial.println("\nConnected to WiFi");
  
  device_id = WiFi.macAddress();
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
  // result: "2024/05/15/14/32/07"
}

void connectToMQTT() {
  while (!mqtt_client.connected()) {
    String client_id = "esp32-client-" + String(WiFi.macAddress());
    Serial.printf("Connecting to MQTT Broker as %s...\n", client_id.c_str());
    if (mqtt_client.connect(client_id.c_str(), mqtt_username, mqtt_password)) {
      Serial.println("Connected to MQTT broker");
      mqtt_client.subscribe((base_topic + "config").c_str());
      mqtt_client.subscribe((base_topic + "flag").c_str());
      mqtt_client.subscribe((base_topic + "price").c_str());
      mqtt_client.subscribe((base_topic + "sensordata").c_str());
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

    // Parse JSON
    StaticJsonDocument<200> doc;  // 200 = max bytes for the JSON
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        return;
    }
  
  if (String(topic) == (base_topic + "price")){
    price = doc["price"];

    lcd.setCursor(2,0);
    char buf[10];
    dtostrf(price, 1, 2, buf);
    lcd.print(buf);
    lcd.print("     ");
  }

  if (String(topic) == (base_topic + "config")){
    samplingInterval = doc["samplingInterval"];
    T_min = doc["T_min"];
    T_max = doc["T_max"];
    H_min = doc["H_min"];
    H_max = doc["H_max"];
    showsensordata = doc["showsensordata"];

  }

  if (String(topic) == (base_topic + "flag")){
    flag = doc["flag"];
  }
}

void loop() {
    if (!mqtt_client.connected()) {
        connectToMQTT();
    }
    mqtt_client.loop();

    if (millis() - lastPublish >= samplingInterval) {
      lastPublish = millis();

      int chk = DHT11.read(DHT11PIN);
      String humidity_reading = String(DHT11.humidity);
      String temperature_reading = String(DHT11.temperature);
      String timestamp = getTimestamp();
      String msg = "{\"time\":" + timestamp + ",\"temperature\":" + temperature_reading + ",\"humidity\":" + humidity_reading + "}";
      mqtt_client.publish((base_topic + "sensordata").c_str(), msg.c_str());

        if (!flag){
          if (!(T_min < (float)DHT11.temperature && (float)DHT11.temperature < T_max) || !(H_min < (float)DHT11.humidity && (float)DHT11.humidity < H_max)){
            digitalWrite(rled, HIGH);
          } else {
            digitalWrite(rled, LOW);
          }
        } else {
          digitalWrite(rled, HIGH);
        }

        lcd.setCursor(2, 0);
        char buf[10];
        dtostrf(price, 1, 2, buf);
        lcd.print(buf);
        lcd.print("     ");
        if (showsensordata){
          lcd.setCursor(0,1);
          lcd.print("T:");
          lcd.print(DHT11.temperature);
          lcd.write(223);
          lcd.print("C H:");
          lcd.print(DHT11.humidity);
          lcd.print("%");
        } else {
          lcd.setCursor(0,1);
          lcd.print("                ");
        }
    }
}
