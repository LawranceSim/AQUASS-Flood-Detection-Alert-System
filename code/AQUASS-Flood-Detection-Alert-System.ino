// -------- Blynk Credentials --------
#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "Flood Monitor"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"

// -------- Telegram Credentials --------
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID "YOUR_CHAT_ID"                 

// -------- WiFi Credentials --------
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

// -------- Libraries --------
#define BLYNK_PRINT Serial
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiClientSecure.h>     
#include <UniversalTelegramBot.h>  
#include <ArduinoJson.h>           

// -------- Pin definitions --------
#define TRIG_PIN D5
#define ECHO_PIN D6

#define RED_LED    D4
#define YELLOW_LED D0
#define GREEN_LED  D7

#define RELAY_PIN  D3 

#define VPIN_WATER_LEVEL V10

// -------- Thresholds (Water Depth) --------
// Note: We use "Depth" logic now. Higher number = More water.
#define DANGER_DEPTH    30   // Danger when water is 30cm deep
#define WARNING_DEPTH   15   // Warning when water is 15cm deep
#define NORMAL_HEIGHT   60   // Sensor height from bottom of tank
#define DANGER_COUNT_THRESHOLD 20 // 2 seconds confirmation

// -------- Filter settings --------
#define MEDIAN_SIZE 9    
#define AVG_SIZE    3

// -------- Objects --------
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClientSecure client;                    
UniversalTelegramBot bot(BOT_TOKEN, client);

// -------- Variables --------
long duration;
float rawDistance;
float medianDistance;
float filteredDistance;
float currentLevel; // Current water depth
int dangerCount = 0;

// Filter Buffers
float medianBuffer[MEDIAN_SIZE];
int medianIndex = 0;
bool medianFilled = false;

float avgBuffer[AVG_SIZE];
int avgIndex = 0;
bool avgFilled = false;

// Timers
unsigned long lastSensorRead = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastTelegramTime = 0;
const int SENSOR_INTERVAL = 100; 
const int TELEGRAM_COOLDOWN = 15000; 
unsigned long lastBlynkUpdate = 0;

// States
enum SystemState { STATE_SAFE, STATE_WARNING, STATE_DANGER };
SystemState currentState = STATE_SAFE;
SystemState lastSentState = STATE_SAFE;

// -------- Generic median filter --------
float medianFilter(float *arr, int size) {
  float temp[size];
  for(int k=0; k<size; k++) temp[k] = arr[k];
  for (int i = 0; i < size - 1; i++) {
    for (int j = i + 1; j < size; j++) {
      if (temp[j] < temp[i]) {
        float t = temp[i];
        temp[i] = temp[j];
        temp[j] = t;
      }
    }
  }
  return temp[size / 2];
}

void setup() {
  Serial.begin(9600);

  // Hardware Pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Off

  // LCD
  Wire.begin(D2, D1);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Starting");
  
  // Network
  client.setInsecure();  
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  lcd.clear();
  lcd.print("Connected!");
  delay(1000);
  lcd.clear();
  
  bot.sendMessage(CHAT_ID, "System Online: Full Fixed Code.", ""); 
}

void loop() {
  Blynk.run();
  unsigned long now = millis();

  // -------- Sensor Read --------
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    duration = pulseIn(ECHO_PIN, HIGH, 25000); 

    if (duration > 0) {
      rawDistance = (duration * 0.0343) / 2;

      // Filtering
      medianBuffer[medianIndex++] = rawDistance;
      if (medianIndex >= MEDIAN_SIZE) { medianIndex = 0; medianFilled = true; }

      if (medianFilled) medianDistance = medianFilter(medianBuffer, MEDIAN_SIZE);
      else medianDistance = rawDistance;

      avgBuffer[avgIndex++] = medianDistance;
      if (avgIndex >= AVG_SIZE) { avgIndex = 0; avgFilled = true; }

      float sum = 0;
      int count = avgFilled ? AVG_SIZE : avgIndex;
      for (int i = 0; i < count; i++) sum += avgBuffer[i];
      filteredDistance = (count > 0) ? sum / count : medianDistance;
    }
    
    // -------- Logic Calculation --------
    // Calculate Water Depth (Height - Distance)
    currentLevel = NORMAL_HEIGHT - filteredDistance;

    //Data transmitting and receiving
    //send currentLevel value real time
    if (now - lastBlynkUpdate >= 500) {   // update graph every 500 ms
      lastBlynkUpdate = now;
      Blynk.virtualWrite(VPIN_WATER_LEVEL, currentLevel);
    }


    if (currentLevel < 0) currentLevel = 0; 

    // -------- State Logic --------
    // 1. Check DANGER first (Must check highest value first)
    if (currentLevel >= DANGER_DEPTH) {
      dangerCount++;

      if (dangerCount >= DANGER_COUNT_THRESHOLD) {
        currentState = STATE_DANGER;
        digitalWrite(RED_LED, HIGH);
        digitalWrite(YELLOW_LED, LOW);
        digitalWrite(GREEN_LED, LOW);
        digitalWrite(RELAY_PIN, LOW); // Siren ON
      }
    } 
    else {
      // If we are NOT in danger, reset the counter
      dangerCount = 0; 

      // 2. Check WARNING
      if (currentLevel >= WARNING_DEPTH) {
        currentState = STATE_WARNING;
        digitalWrite(RED_LED, LOW);
        digitalWrite(YELLOW_LED, HIGH);
        digitalWrite(GREEN_LED, LOW);
        digitalWrite(RELAY_PIN, HIGH); // Siren OFF
      } 
      // 3. SAFE
      else {
        currentState = STATE_SAFE;
        digitalWrite(RED_LED, LOW);
        digitalWrite(YELLOW_LED, LOW);
        digitalWrite(GREEN_LED, HIGH);
        digitalWrite(RELAY_PIN, HIGH); // Siren OFF
      }
    }
  }


  

  // -------- Notifications --------
  if (currentState != lastSentState) {
    
    bool timeExpired = (now - lastTelegramTime > TELEGRAM_COOLDOWN);
    
    // Send if DANGER (Immediate) OR WARNING (Throttled)
    if (currentState == STATE_DANGER || (currentState == STATE_WARNING && timeExpired)) {
      
      if (currentState == STATE_WARNING) {
        Blynk.logEvent("warning_alert", "Water level rising");
        bot.sendMessage(CHAT_ID, "⚠️ WARNING: Water level rising.", "");
        lastTelegramTime = now; 
      }
      else if (currentState == STATE_DANGER) {
        Blynk.logEvent("danger_alert", "CRITICAL flood detected");
        bot.sendMessage(CHAT_ID, "🚨 FLOOD ALERT 🚨 Evacuate immediately!", "");
        lastTelegramTime = now; 
      }
      lastSentState = currentState;
    }
    
    // Reset state tracker if we go back to Safe
    if (currentState == STATE_SAFE) {
       lastSentState = STATE_SAFE;
    }
  }

  // -------- LCD Update --------
  if (now - lastLcdUpdate >= 500) {
    lastLcdUpdate = now;

    lcd.setCursor(0, 0);
    lcd.print("W.Lvl: "); // Displaying Water Level now
    lcd.print(currentLevel, 1);
    lcd.print(" cm  ");

    lcd.setCursor(0, 1);
    lcd.print("S.Dist: "); // Displaying sensor reading now
    lcd.print(filteredDistance, 1);
    lcd.print(" cm  ");
  }
}
