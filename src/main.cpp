#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#if defined(ESP32)
    #include <WiFi.h>
    #define SSL_CLIENT WiFiClientSecure
#elif defined(ESP8266)
    #include <ESP8266WiFi.h>
    #define SSL_CLIENT WiFiClientSecure
#endif

#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "ExampleFunctions.h"
#include <time.h>

// --------------------- USER CONFIG ---------------------
#define WIFI_SSID     "Galaxy S24+"
#define WIFI_PASSWORD "1234567890"

#define API_KEY       "AIzaSyBUtc9tqnY-HiRJ1XhKFyaBlaHuSFNwGaQ"
#define DATABASE_URL  "https://fish-feeder-8f8cf-default-rtdb.europe-west1.firebasedatabase.app"
#define USER_EMAIL    "chiraandrei222@gmail.com"
#define USER_PASS     "123456"

// NTP Server for time sync (adjust for Romania: UTC+2 or UTC+3 DST)
#define NTP_SERVER    "pool.ntp.org"
#define GMT_OFFSET    7200      // UTC+2 in seconds
#define DST_OFFSET    3600      // DST offset (summer time)

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
const int SERVO_SPEED = 0;           
const int SERVO_STOP = 90;           
const int ROTATION_TIME_MS = 500; 

// Turbidity sensor
int turbidityThreshold = 500;
bool alertSent = false;

// Timer storage
struct FeedTimer {
  String time;      // "HH:MM"
  bool enabled;
  bool triggeredToday;
  String key;       // Firebase key (e.g., "timer0")
};
FeedTimer timers[5];
int timerCount = 0;

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

// Track last hour for daily reset
int lastHour = -1;

// ==================== FORWARD DECLARATIONS ====================
void processData(AsyncResult &aResult);
void feedFish(const char* reason);
void readTurbiditySensor();
void updateDeviceStatus();
void checkTimers();
String getCurrentTimestamp();
String getCurrentTime();
int getCurrentHour();
void resetDailyTriggers();
void initWiFi();
void initTime();

// ==================== UTILITY FUNCTIONS ====================

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

// Reset daily triggers (call at midnight)
void resetDailyTriggers() {
  Serial.println("Midnight - deleting all timers");
  
  // Delete all timers from Firebase
  if (firebaseReady) {
    Database.remove(aClient, "/timers", processData, "deleteAllTimers");
  }
  
  // Clear local timer array
  for (int i = 0; i < 5; i++) {
    timers[i].time = "";
    timers[i].enabled = false;
    timers[i].triggeredToday = false;
    timers[i].key = "";
  }
  timerCount = 0;
}

// Connect WiFi
void initWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println("\nWiFi Connected!");
  digitalWrite(LED_BUILTIN, HIGH);
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

// ==================== FEED FUNCTION ====================

// Feed the fish - rotate servo 180 degrees
void feedFish(const char* reason) {
  Serial.printf(">>> FEEDING FISH: %s\n", reason);
  
  // Rotate to feed position
  servoMotor.write(SERVO_SPEED);
  delay(ROTATION_TIME_MS);
  servoMotor.write(SERVO_STOP);

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

// ==================== SENSOR FUNCTIONS ====================

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

// ==================== TIMER FUNCTIONS ====================

// Check if any timer should trigger feeding
void checkTimers() {
  String currentTime = getCurrentTime();
  
  for (int i = 0; i < timerCount; i++) {
    if (timers[i].enabled && !timers[i].triggeredToday) {
      if (timers[i].time == currentTime) {
        timers[i].triggeredToday = true;
        char reason[64];
        snprintf(reason, sizeof(reason), "Timer %s (%s)", timers[i].key.c_str(), timers[i].time.c_str());
        feedFish(reason);
        
        // Update triggered status in Firebase
        if (firebaseReady && timers[i].key.length() > 0) {
          String path = "/timers/" + timers[i].key + "/triggered";
          Database.set<bool>(aClient, path.c_str(), true, processData, "setTriggered");
        }
      }
    }
  }
}

// ==================== FIREBASE CALLBACK ====================

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
    JsonDocument doc;
    if (deserializeJson(doc, RTDB.to<String>()) == DeserializationError::Ok) {
      
      // Load feedCount
      if (doc["feedCount"].is<int>()) {
        feedCount = doc["feedCount"].as<int>();
        Serial.printf("Loaded feedCount: %d\n", feedCount);
      }
      
      // Load turbidity threshold
      if (doc["turbidity"]["threshold"].is<int>()) {
        turbidityThreshold = doc["turbidity"]["threshold"].as<int>();
        Serial.printf("Loaded turbidity threshold: %d\n", turbidityThreshold);
      }
      
      // Load timers dynamically (any timerX key, max 5)
      if (doc["timers"].is<JsonObject>()) {
        JsonObject timersObj = doc["timers"].as<JsonObject>();
        timerCount = 0;
        for (JsonPair kv : timersObj) {
          if (timerCount >= 5) break;
          String key = kv.key().c_str();
          if (key.startsWith("timer")) {
            JsonObject timerObj = kv.value().as<JsonObject>();
            timers[timerCount].key = key;
            timers[timerCount].time = timerObj["time"].as<String>();
            timers[timerCount].enabled = timerObj["enabled"].as<bool>();
            timers[timerCount].triggeredToday = timerObj["triggered"].as<bool>();
            Serial.printf("Loaded %s: %s, enabled: %s, triggered: %s\n", 
                          key.c_str(),
                          timers[timerCount].time.c_str(), 
                          timers[timerCount].enabled ? "yes" : "no",
                          timers[timerCount].triggeredToday ? "yes" : "no");
            timerCount++;
          }
        }
        Serial.printf("Total timers loaded: %d\n", timerCount);
      }
      
      // Check feednow on startup
      if (doc["feednow"].is<bool>() && doc["feednow"].as<bool>()) {
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
    // Extract timer key from path like "/timers/timer0" or "/timers/timer0/time"
    int secondSlash = path.indexOf('/', 8); // After "/timers/"
    String timerKey;
    if (secondSlash > 0) {
      timerKey = path.substring(8, secondSlash);
    } else {
      timerKey = path.substring(8);
    }
    
    // Find or create timer entry
    int timerIndex = -1;
    for (int i = 0; i < timerCount; i++) {
      if (timers[i].key == timerKey) {
        timerIndex = i;
        break;
      }
    }
    
    // If not found and room available, add new timer
    if (timerIndex < 0 && timerCount < 5) {
      timerIndex = timerCount;
      timers[timerIndex].key = timerKey;
      timers[timerIndex].time = "";
      timers[timerIndex].enabled = false;
      timers[timerIndex].triggeredToday = false;
      timerCount++;
    }
    
    if (timerIndex >= 0) {
      if (path.endsWith("/time")) {
        timers[timerIndex].time = RTDB.to<String>();
        Serial.printf("%s time updated: %s\n", timerKey.c_str(), timers[timerIndex].time.c_str());
      }
      else if (path.endsWith("/enabled")) {
        timers[timerIndex].enabled = RTDB.to<bool>();
        Serial.printf("%s enabled: %s\n", timerKey.c_str(), timers[timerIndex].enabled ? "yes" : "no");
      }
      else if (path.endsWith("/triggered")) {
        timers[timerIndex].triggeredToday = RTDB.to<bool>();
        Serial.printf("%s triggered: %s\n", timerKey.c_str(), timers[timerIndex].triggeredToday ? "yes" : "no");
      }
      else if (dataType == 6) {
        // Full timer object update
        JsonDocument doc;
        if (deserializeJson(doc, RTDB.to<String>()) == DeserializationError::Ok) {
          if (doc["time"].is<String>()) {
            timers[timerIndex].time = doc["time"].as<String>();
          }
          if (doc["enabled"].is<bool>()) {
            timers[timerIndex].enabled = doc["enabled"].as<bool>();
          }
          if (doc["triggered"].is<bool>()) {
            timers[timerIndex].triggeredToday = doc["triggered"].as<bool>();
          }
          Serial.printf("%s updated: %s, enabled: %s, triggered: %s\n", timerKey.c_str(),
                        timers[timerIndex].time.c_str(),
                        timers[timerIndex].enabled ? "yes" : "no",
                        timers[timerIndex].triggeredToday ? "yes" : "no");
        }
      }
    }
  }
  
  // Handle timer deletions (when timers path becomes null or a timer is deleted)
  else if (path == "/timers" && RTDB.to<String>() == "null") {
    Serial.println("All timers deleted");
    for (int i = 0; i < 5; i++) {
      timers[i].time = "";
      timers[i].enabled = false;
      timers[i].triggeredToday = false;
      timers[i].key = "";
    }
    timerCount = 0;
  }
  
  // Feed count changed externally
  else if (path == "/feedCount") {
    feedCount = RTDB.to<int>();
    Serial.printf("Feed count synced: %d\n", feedCount);
  }

  Serial.println("-------------------------");
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(300);

  // Setup servo motor
  servoMotor.attach(SERVO_PIN);
  servoMotor.write(SERVO_STOP);
  Serial.println("Servo motor initialized on GPIO25");

  // Setup turbidity sensor pin
  pinMode(TURBIDITY_PIN, INPUT);
  Serial.println("Turbidity sensor initialized on GPIO34");

  // Built-in LED for status
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Initialize timers
  for (int i = 0; i < 5; i++) {
    timers[i].time = "";
    timers[i].enabled = false;
    timers[i].triggeredToday = false;
    timers[i].key = "";
  }
  timerCount = 0;

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  initWiFi();
  
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

// ==================== LOOP ====================

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
  int currentHour = getCurrentHour();
  if (currentHour == 0 && lastHour == 23) {
    resetDailyTriggers();
  }
  lastHour = currentHour;
}