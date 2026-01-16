#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#if defined(ESP32)
    #include <WiFi.h>
#elif defined(ESP8266)
    #include <ESP8266WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include "ExampleFunctions.h"
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// --------------------- USER CONFIG ---------------------
#define WIFI_SSID     "Camera 105"
#define WIFI_PASSWORD "Camera1052025"

#define API_KEY       "AIzaSyBUtc9tqnY-HiRJ1XhKFyaBlaHuSFNwGaQ"
#define DATABASE_URL  "https://fish-feeder-8f8cf-default-rtdb.europe-west1.firebasedatabase.app"
#define USER_EMAIL    "chiraandrei222@gmail.com"
#define USER_PASS     "123456"
// --------------------------------------------------------

// Firebase Authentication
UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASS);

SSL_CLIENT ssl_client, stream_ssl_client;

// Firebase Components
FirebaseApp app;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client), streamClient(stream_ssl_client);
RealtimeDatabase Database;

// GPIO map
const int GPIO1 = 12;
const int GPIO2 = 13;
const int GPIO3 = 14;
const int GPIO34_SERVO = 34;

// Servo motor control
Servo servoMotor;
const int SERVO_STOP_ANGLE = 90;    // Neutral position (stopped)
const int SERVO_START_ANGLE = 0;    // Or set to 180 depending on your servo direction

// Firebase listening paths
String path_gpio1 = "/gpio1";
String path_gpio2 = "/gpio2";
String path_gpio3 = "/gpio3";
String path_gpio34 = "/gpio34";

// Callback function (handles stream events)
void processData(AsyncResult &aResult);

// Connect WiFi
void initWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println("\nWiFi Connected!");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Setup GPIO pins
  pinMode(GPIO1, OUTPUT);
  pinMode(GPIO2, OUTPUT);
  pinMode(GPIO3, OUTPUT);
  digitalWrite(GPIO1, LOW);
  digitalWrite(GPIO2, LOW);
  digitalWrite(GPIO3, LOW);

  // Setup servo motor on GPIO34
  servoMotor.attach(GPIO34_SERVO);
  servoMotor.write(SERVO_STOP_ANGLE);  // Start in stopped position
  Serial.println("Servo motor initialized on GPIO34");

  initWiFi();

  // Configure SSL
  ssl_client.setInsecure();
  stream_ssl_client.setInsecure();

  // Initialize Firebase (Authentication)
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  // Stream setup
  streamClient.setSSEFilters("put,patch,keep-alive,cancel,auth_revoked");

  // Listen to 3 keys
  Database.get(streamClient, "/", processData, true /* SSE */, "streamTask");

  Serial.println("Firebase streaming initialized...");
}

void loop() {
  app.loop();
}

// STREAM CALLBACK
void processData(AsyncResult &aResult) {

  if (!aResult.isResult()) return;

  if (aResult.isError()) {
    Serial.printf("Firebase Error: %s | Code: %d\n",
                  aResult.error().message().c_str(),
                  aResult.error().code());
  }

  if (!aResult.available()) return;

  RealtimeDatabaseResult &RTDB = aResult.to<RealtimeDatabaseResult>();

  if (!RTDB.isStream()) return;

  Serial.println("----- Stream Update -----");
  Serial.printf("Event: %s\n", RTDB.event().c_str());
  Serial.printf("Path : %s\n", RTDB.dataPath().c_str());
  Serial.printf("Data : %s\n", RTDB.to<String>().c_str());

  int dataType = RTDB.type();   // 1 = integer, 4 = boolean, 6 = JSON

  String fullPath = RTDB.dataPath(); // e.g. "/gpio1"

  // If full JSON received on first stream
  if (dataType == 6) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, RTDB.to<String>()) == DeserializationError::Ok) {
      if (doc.containsKey("gpio1"))
        digitalWrite(GPIO1, doc["gpio1"].as<int>() ? HIGH : LOW);

      if (doc.containsKey("gpio2"))
        digitalWrite(GPIO2, doc["gpio2"].as<int>() ? HIGH : LOW);

      if (doc.containsKey("gpio3"))
        digitalWrite(GPIO3, doc["gpio3"].as<int>() ? HIGH : LOW);

      if (doc.containsKey("gpio34")) {
        int servoCommand = doc["gpio34"].as<int>();
        if (servoCommand == 0) {
          servoMotor.write(SERVO_STOP_ANGLE);
          Serial.println("Servo stopped");
        } else {
          servoMotor.write(SERVO_START_ANGLE);
          Serial.println("Servo started");
        }
      }
    }
    return;
  }

  // Single key update:
  int value = RTDB.to<int>(); // database stores 0/1
  if (fullPath == "/gpio1") digitalWrite(GPIO1, value ? HIGH : LOW);
  if (fullPath == "/gpio2") digitalWrite(GPIO2, value ? HIGH : LOW);
  if (fullPath == "/gpio3") digitalWrite(GPIO3, value ? HIGH : LOW);
  if (fullPath == "/gpio34") {
    if (value == 0) {
      servoMotor.write(SERVO_STOP_ANGLE);
      Serial.println("Servo stopped via update");
    } else {
      servoMotor.write(SERVO_START_ANGLE);
      Serial.println("Servo started via update");
    }
  }

  Serial.println("GPIO Updated.");
}