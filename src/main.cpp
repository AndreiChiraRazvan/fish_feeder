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
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <time.h>
#include "ExampleFunctions.h"

// --------------------- USER CONFIG ---------------------
#define WIFI_SSID     "Camera 105"
#define WIFI_PASSWORD "Camera1052025"

#define API_KEY       "AIzaSyBUtc9tqnY-HiRJ1XhKFyaBlaHuSFNwGaQ"
#define DATABASE_URL  "https://fish-feeder-8f8cf-default-rtdb.europe-west1.firebasedatabase.app"
#define USER_EMAIL    "chiraandrei222@gmail.com"
#define USER_PASS     "123456"

// NTP Server for time sync (adjust for Romania: UTC+2 or UTC+3 DST)
#define NTP_SERVER    "pool.ntp.org"
#define GMT_OFFSET    7200      // UTC+2 in seconds
#define DST_OFFSET    3600      // DST offset (summer time)
// --------------------------------------------------------

// Firebase Authentication
UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASS);

SSL_CLIENT ssl_client, stream_ssl_client;

// Firebase Components
FirebaseApp app;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client), streamClient(stream_ssl_client);
RealtimeDatabase Database;

// GPIO Configuration
const int SERVO_PIN = 25;
const int TURBIDITY_PIN = 34;

// Servo motor control
Servo servoMotor;
const int SERVO_IDLE_ANGLE = 0;
const int SERVO_FEED_ANGLE = 180;
const int FEED_DURATION_MS = 1000;  // Time to hold at feed position

// Turbidity sensor
int turbidityThreshold = 500;
bool alertSent = false;

// Timer storage
struct FeedTimer {
  String time;      // "HH:MM"
  bool enabled;
  bool triggeredToday;
};
FeedTimer timers[3];

// Timing variables
unsigned long lastTurbidityRead = 0;
unsigned long lastDeviceUpdate = 0;
unsigned long lastTimerCheck = 0;
const unsigned long TURBIDITY_INTERVAL = 5000;    // Read sensor every 5 seconds
const unsigned long DEVICE_UPDATE_INTERVAL = 60000; // Update device status every minute
const unsigned long TIMER_CHECK_INTERVAL = 1000;   // Check timers every second

// Feed count from database
int feedCount = 0;

// Firebase ready flag
bool firebaseReady = false;

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

// Initialize NTP time
void initTime() {
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  Serial.print("Waiting for NTP time sync");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synchronized!");
  Serial.printf("Current time: %s\n", getCurrentTimestamp().c_str());
}

// Get current timestamp in ISO format
String getCurrentTimestamp() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", timeinfo);
  return String(buffer);
}

// Get current time as "HH:MM"
String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", timeinfo);
  return String(buffer);
}

// Get current hour for daily reset check
int getCurrentHour() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  return timeinfo->tm_hour;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Setup servo motor
  servoMotor.attach(SERVO_PIN);
  servoMotor.write(SERVO_IDLE_ANGLE);
  Serial.println("Servo motor initialized on GPIO25");

  // Setup turbidity sensor pin
  pinMode(TURBIDITY_PIN, INPUT);
  Serial.println("Turbidity sensor initialized on GPIO34");

  // Built-in LED for status
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Initialize timers
  for (int i = 0; i < 3; i++) {
    timers[i].time = "";
    timers[i].enabled = false;
    timers[i].triggeredToday = false;
  }

  initWiFi();
  digitalWrite(LED_BUILTIN, HIGH);
  
  initTime();

  // Configure SSL
  ssl_client.setInsecure();
  stream_ssl_client.setInsecure();

  // Initialize Firebase
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  // Stream setup - listen to entire database
  streamClient.setSSEFilters("put,patch,keep-alive,cancel,auth_revoked");
  Database.get(streamClient, "/", processData, true, "streamTask");

  Serial.println("Firebase streaming initialized...");
}

void loop() {
  app.loop();

  unsigned long currentMillis = millis();

  // Read turbidity sensor periodically
  if (currentMillis - lastTurbidityRead >= TURBIDITY_INTERVAL) {
    lastTurbidityRead = currentMillis;
    readTurbiditySensor();
  }

  // Update device status periodically
  if (currentMillis - lastDeviceUpdate >= DEVICE_UPDATE_INTERVAL) {
    lastDeviceUpdate = currentMillis;
    updateDeviceStatus();
  }

  // Check timers
  if (currentMillis - lastTimerCheck >= TIMER_CHECK_INTERVAL) {
    lastTimerCheck = currentMillis;
    checkTimers();
  }

  // Reset daily triggers at midnight
  static int lastHour = -1;
  int currentHour = getCurrentHour();
  if (currentHour == 0 && lastHour == 23) {
    resetDailyTriggers();
  }
  lastHour = currentHour;
}

// Feed the fish - rotate servo 180 degrees
void feedFish(const char* reason) {
  Serial.printf(">>> FEEDING FISH: %s\n", reason);
  
  // Rotate to feed position
  servoMotor.write(SERVO_FEED_ANGLE);
  delay(FEED_DURATION_MS);
  
  // Return to idle position
  servoMotor.write(SERVO_IDLE_ANGLE);
  
  // Update feed count
  feedCount++;
  
  // Update database
  if (firebaseReady) {
    String timestamp = getCurrentTimestamp();
    
    // Update feedCount
    Database.set<int>(aClient, "/feedCount", feedCount, processData, "updateFeedCount");
    
    // Update lastFed timestamp
    Database.set<String>(aClient, "/lastFed", timestamp, processData, "updateLastFed");
    
    // Reset feednow to false
    Database.set<bool>(aClient, "/feednow", false, processData, "resetFeedNow");
  }
  
  Serial.printf("Feed complete. Total feeds: %d\n", feedCount);
}

// Read turbidity sensor and update database
void readTurbiditySensor() {
  int sensorValue = analogRead(TURBIDITY_PIN);
  
  Serial.printf("Turbidity sensor value: %d (threshold: %d)\n", sensorValue, turbidityThreshold);
  
  if (!firebaseReady) return;
  
  // Update sensor value in database
  Database.set<int>(aClient, "/turbidity/value", sensorValue, processData, "updateTurbidity");
  
  // Update timestamp
  String timestamp = getCurrentTimestamp();
  Database.set<String>(aClient, "/turbidity/lastUpdate", timestamp, processData, "updateTurbidityTime");
  
  // Check threshold and send alert if needed
  bool shouldAlert = sensorValue > turbidityThreshold;
  
  if (shouldAlert && !alertSent) {
    Database.set<bool>(aClient, "/turbidity/alert", true, processData, "setAlert");
    alertSent = true;
    Serial.println("!!! TURBIDITY ALERT: Water quality issue detected!");
  } else if (!shouldAlert && alertSent) {
    Database.set<bool>(aClient, "/turbidity/alert", false, processData, "clearAlert");
    alertSent = false;
    Serial.println("Turbidity alert cleared - water quality normal");
  }
}

// Update device online status
void updateDeviceStatus() {
  if (!firebaseReady) return;
  
  String timestamp = getCurrentTimestamp();
  Database.set<bool>(aClient, "/device/online", true, processData, "updateOnline");
  Database.set<String>(aClient, "/device/lastSeen", timestamp, processData, "updateLastSeen");
  
  Serial.printf("Device status updated: %s\n", timestamp.c_str());
}

// Check if any timer should trigger feeding
void checkTimers() {
  String currentTime = getCurrentTime();
  
  for (int i = 0; i < 3; i++) {
    if (timers[i].enabled && !timers[i].triggeredToday) {
      if (timers[i].time == currentTime) {
        timers[i].triggeredToday = true;
        char reason[32];
        snprintf(reason, sizeof(reason), "Timer %d (%s)", i, timers[i].time.c_str());
        feedFish(reason);
      }
    }
  }
}

// Reset daily triggers (call at midnight)
void resetDailyTriggers() {
  Serial.println("Midnight - resetting daily triggers");
  for (int i = 0; i < 3; i++) {
    timers[i].triggeredToday = false;
  }
}

// Firebase stream callback
void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;

  if (aResult.isError()) {
    Serial.printf("Firebase Error: %s | Code: %d\n",
                  aResult.error().message().c_str(),
                  aResult.error().code());
    return;
  }

  // Mark Firebase as ready after successful auth
  if (aResult.uid() == "authTask") {
    firebaseReady = true;
    Serial.println("Firebase authenticated successfully!");
    updateDeviceStatus();
    return;
  }

  if (!aResult.available()) return;

  RealtimeDatabaseResult &RTDB = aResult.to<RealtimeDatabaseResult>();

  if (!RTDB.isStream()) return;

  Serial.println("----- Stream Update -----");
  Serial.printf("Event: %s\n", RTDB.event().c_str());
  Serial.printf("Path : %s\n", RTDB.dataPath().c_str());
  Serial.printf("Data : %s\n", RTDB.to<String>().c_str());

  String path = RTDB.dataPath();
  int dataType = RTDB.type();

  // Handle full JSON on initial stream connection
  if (dataType == 6 && path == "/") {
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, RTDB.to<String>()) == DeserializationError::Ok) {
      
      // Load feedCount
      if (doc.containsKey("feedCount")) {
        feedCount = doc["feedCount"].as<int>();
        Serial.printf("Loaded feedCount: %d\n", feedCount);
      }
      
      // Load turbidity threshold
      if (doc.containsKey("turbidity") && doc["turbidity"].containsKey("threshold")) {
        turbidityThreshold = doc["turbidity"]["threshold"].as<int>();
        Serial.printf("Loaded turbidity threshold: %d\n", turbidityThreshold);
      }
      
      // Load timers
      if (doc.containsKey("timers")) {
        JsonObject timersObj = doc["timers"].as<JsonObject>();
        for (int i = 0; i < 3; i++) {
          char timerKey[8];
          snprintf(timerKey, sizeof(timerKey), "timer%d", i);
          if (timersObj.containsKey(timerKey)) {
            timers[i].time = timersObj[timerKey]["time"].as<String>();
            timers[i].enabled = timersObj[timerKey]["enabled"].as<bool>();
            Serial.printf("Timer %d: %s, enabled: %s\n", i, 
                          timers[i].time.c_str(), 
                          timers[i].enabled ? "yes" : "no");
          }
        }
      }
      
      // Check feednow on startup
      if (doc.containsKey("feednow") && doc["feednow"].as<bool>()) {
        feedFish("Feed Now (startup)");
      }
    }
    return;
  }

  // Handle individual path updates
  
  // Feed now triggered
  if (path == "/feednow") {
    bool feedNow = RTDB.to<bool>();
    if (feedNow) {
      feedFish("Feed Now (manual)");
    }
  }
  
  // Turbidity threshold changed
  else if (path == "/turbidity/threshold") {
    turbidityThreshold = RTDB.to<int>();
    Serial.printf("Turbidity threshold updated: %d\n", turbidityThreshold);
  }
  
  // Timer updates
  else if (path.startsWith("/timers/timer")) {
    // Extract timer index from path like "/timers/timer0" or "/timers/timer0/time"
    int timerIndex = path.charAt(14) - '0';  // Get the digit after "timer"
    
    if (timerIndex >= 0 && timerIndex < 3) {
      if (path.endsWith("/time")) {
        timers[timerIndex].time = RTDB.to<String>();
        Serial.printf("Timer %d time updated: %s\n", timerIndex, timers[timerIndex].time.c_str());
      }
      else if (path.endsWith("/enabled")) {
        timers[timerIndex].enabled = RTDB.to<bool>();
        Serial.printf("Timer %d enabled: %s\n", timerIndex, timers[timerIndex].enabled ? "yes" : "no");
      }
      else if (dataType == 6) {
        // Full timer object update
        DynamicJsonDocument doc(128);
        if (deserializeJson(doc, RTDB.to<String>()) == DeserializationError::Ok) {
          if (doc.containsKey("time")) {
            timers[timerIndex].time = doc["time"].as<String>();
          }
          if (doc.containsKey("enabled")) {
            timers[timerIndex].enabled = doc["enabled"].as<bool>();
          }
          Serial.printf("Timer %d updated: %s, enabled: %s\n", timerIndex,
                        timers[timerIndex].time.c_str(),
                        timers[timerIndex].enabled ? "yes" : "no");
        }
      }
    }
  }
  
  // Feed count changed externally
  else if (path == "/feedCount") {
    feedCount = RTDB.to<int>();
    Serial.printf("Feed count synced: %d\n", feedCount);
  }

  Serial.println("-------------------------");
}