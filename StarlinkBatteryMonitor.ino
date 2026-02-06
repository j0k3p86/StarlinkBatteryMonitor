#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include "secrets.h"

// --- Configuration ---
const unsigned long ALERT_INTERVAL = 600000; // 10 Minutes between alerts (ms)
const unsigned long REPORT_INTERVAL = 3600000; // 1 Hour between reports (ms)
const unsigned long VOLTAGE_READ_INTERVAL = 2000; // Read voltage every 2 seconds

const int ANALOG_PIN = A0;
const float R1 = 100000.0; // 100k Ohms
const float R2 = 22000.0;  // 22k Ohms
// Reference voltage of D1 Mini ADC input max (Adjust if needed, typically 3.2V or 3.3V)
const float V_REF = 3.3;   
const float LOW_VOLTAGE_THRESHOLD = 11.0;
const float CRITICAL_VOLTAGE_THRESHOLD = 10.0;

// Divider Factor: V_in = V_out * (R1 + R2) / R2
const float DIVIDER_RATIO = (R1 + R2) / R2;

// Smoothing
const int NUM_SAMPLES = 20;
const unsigned long SAMPLE_INTERVAL = 10; // 10ms between samples

// Global State
float currentVoltage = 0.0;
float calibrationFactor = 1.0;
unsigned long lastSampleTime = 0;
long voltageSum = 0;
int sampleCount = 0;

WiFiClientSecure client;
ESP8266WiFiMulti wifiMulti;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Timing variables
unsigned long lastAlertTime = 0;
unsigned long lastReportTime = 0;
unsigned long lastVoltageReadTime = 0;
unsigned long lastBlinkTime = 0;
bool ledState = HIGH; // Start OFF (High is off for built-in LED usually)
bool reportsEnabled = true; // Control periodic reports

// Bot Timing
int botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;

String getUptime() {
  unsigned long millisec = millis();
  unsigned long seconds = millisec / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;
  
  String uptime = String(days) + "d " + String(hours % 24) + "h " + String(minutes % 60) + "m";
  return uptime;
}

String getStatusMessage() {
  String msg = "🔋 **Status Report**\n";
  msg += "Voltage: " + String(currentVoltage, 2) + "V\n";
  
  long rssi = WiFi.RSSI();
  msg += "SSID: " + WiFi.SSID() + "\n";
  msg += "Signal: " + String(rssi) + " dBm\n";
  msg += "Uptime: " + getUptime() + "\n";
  return msg;
}

void saveCalibration() {
  EEPROM.put(0, calibrationFactor);
  EEPROM.commit();
}

void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages");
  Serial.println(String(numNewMessages));

  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Unauthorized user", "");
      continue;
    }

    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (text == "/status") {
      bot.sendMessage(chat_id, getStatusMessage(), "Markdown");
    }

    if (text.startsWith("/calibrate ")) {
      String valueStr = text.substring(11);
      float trueVoltage = valueStr.toFloat();
      
      if (trueVoltage > 5.0 && trueVoltage < 25.0) { // Basic sanity check
        // Calculate new factor
        // current = raw * oldFactor
        // true = raw * newFactor
        // newFactor = true / raw = true / (current / oldFactor) = (true * oldFactor) / current
        
        if (currentVoltage > 1.0) { // Avoid divide by zero
             calibrationFactor = (trueVoltage * calibrationFactor) / currentVoltage;
             saveCalibration();
             
             // Update global voltage immediately so subsequent /status commands show correct value
             currentVoltage = trueVoltage; 
             // Reset sampling buffer to restart with new factor
             voltageSum = 0;
             sampleCount = 0;
             
             bot.sendMessage(chat_id, "✅ Calibration updated. New factor: " + String(calibrationFactor, 4), "");
             // Force immediate update to show new value
             bot.sendMessage(chat_id, "New Voltage: " + String(trueVoltage, 2) + "V", "");
        } else {
             bot.sendMessage(chat_id, "❌ Voltage too low to calibrate.", "");
        }
      } else {
        bot.sendMessage(chat_id, "❌ Invalid voltage. Usage: /calibrate 12.5", "");
      }
    }

    if (text == "/mute") {
      reportsEnabled = false;
      bot.sendMessage(chat_id, "🔕 Periodic reports muted.", "");
    }

    if (text == "/unmute") {
      reportsEnabled = true;
      bot.sendMessage(chat_id, "🔔 Periodic reports unmuted.", "");
    }

    if (text == "/start" || text == "/help") {
      String welcome = "Welcome, " + from_name + ".\n";
      welcome += "Battery Monitor Bot Commands:\n\n";
      welcome += "/status : Get current voltage and signal strength\n";
      welcome += "/mute : Disable periodic reports\n";
      welcome += "/unmute : Enable periodic reports\n";
      welcome += "/calibrate <voltage> : Calibrate sensor (e.g. /calibrate 12.8)\n";
      bot.sendMessage(chat_id, welcome, "Markdown");
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // Ensure off initially

  Serial.begin(115200);
  delay(3000); // Allow Serial to settle
  Serial.println("\n\n--- BATTERY MONITOR SYSTEM STARTING ---");

  // Load Calibration
  EEPROM.begin(64);
  EEPROM.get(0, calibrationFactor);
  if (isnan(calibrationFactor) || calibrationFactor <= 0.1 || calibrationFactor > 10.0) {
    calibrationFactor = 1.0; // Default if invalid
  }
  Serial.print("Calibration Factor: ");
  Serial.println(calibrationFactor);

  // Enable WDT
  ESP.wdtEnable(8000); // 8 seconds hardware watchdog fallback


  // WiFi Setup
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID1, WIFI_PASSWORD1);
  wifiMulti.addAP(WIFI_SSID2, WIFI_PASSWORD2);
  
  Serial.print("Connecting to WiFi");
  
  int retry = 0;
  while (wifiMulti.run() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
  } else {
    Serial.println("\nWiFi Connection Failed! Will try again in loop.");
  }

  // Secure Client Setup
  client.setInsecure(); // Skip certificate validation for simplicity
  
  if (WiFi.status() == WL_CONNECTED) {
    float voltage = readBatteryVoltageBlocking();
    currentVoltage = voltage; // Initialize global
    bot.sendMessage(CHAT_ID, "� **System Started!**", "Markdown");
    bot.sendMessage(CHAT_ID, getStatusMessage(), "Markdown");
  }

  Serial.print("ChatID = ");
  Serial.println(CHAT_ID);

  // OTA Setup
  ArduinoOTA.setHostname("StarlinkBatteryMonitor");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}

void updateVoltageReading() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = currentMillis;
    voltageSum += analogRead(ANALOG_PIN);
    sampleCount++;

    if (sampleCount >= NUM_SAMPLES) {
      float averageReading = (float)voltageSum / NUM_SAMPLES;
      float pinVoltage = averageReading * (V_REF / 1023.0);
      currentVoltage = pinVoltage * DIVIDER_RATIO * calibrationFactor;
      
      // Reset for next batch
      voltageSum = 0;
      sampleCount = 0;
    }
  }
}

// Helper to get fresh reading initially (blocking)
float readBatteryVoltageBlocking() {
  long sum = 0;
  // This is a small blocking delay loop, but 10ms * 20 = 200ms is acceptable.
  // Making this fully non-blocking adds significant state complexity for little gain.
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(ANALOG_PIN);
    delay(10);
  }
  float averageReading = (float)sum / NUM_SAMPLES;
  
  // Calculate voltage at the pin
  float pinVoltage = averageReading * (V_REF / 1023.0);
  
  // Calculate actual battery voltage
  return pinVoltage * DIVIDER_RATIO * calibrationFactor;
}

void handleWiFiReconnection() {
  if (wifiMulti.run() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
  }
}

void loop() {
  ArduinoOTA.handle();
  handleWiFiReconnection();
  
  unsigned long currentMillis = millis();

  // Telegram Bot Handling
  if (currentMillis > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while(numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = currentMillis;
  }

  updateVoltageReading();
  float voltage = currentVoltage;

  // Voltage Monitoring Logic
  if (currentMillis - lastVoltageReadTime >= VOLTAGE_READ_INTERVAL) {
    lastVoltageReadTime = currentMillis;
    
    // voltage is already updated by updateVoltageReading()
    
    Serial.print("Voltage: ");
    Serial.print(voltage);
    Serial.println(" V");

    // Periodic Reporting (Every 10 minutes)
    if (currentMillis - lastReportTime >= REPORT_INTERVAL) {
      if (WiFi.status() == WL_CONNECTED && reportsEnabled) {
        String message = "🔋 Battery Report\nVoltage: " + String(voltage, 2) + "V";
        Serial.println("Sending Periodic Telegram Report...");
        if (bot.sendMessage(CHAT_ID, message, "")) {
           Serial.println("Periodic Report Sent!");
           lastReportTime = currentMillis;
        } else {
           Serial.println("Failed to send Periodic Report");
        }
      }
    }

    if (voltage < CRITICAL_VOLTAGE_THRESHOLD) {
      Serial.println("ALERT: CRITICAL VOLTAGE!");
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > ALERT_INTERVAL) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "🚨 CRITICAL BATTERY LOW! Voltage: " + String(voltage, 2) + "V";
            Serial.println("Sending Telegram Alert...");
            if (bot.sendMessage(CHAT_ID, message, "")) {
              Serial.println("Critical Telegram Alert Sent!");
              lastAlertTime = currentMillis;
            } else {
              Serial.println("Failed to send Critical Telegram Alert");
            }
        }
      }
      
      // Very Fast Blink for Critical State
      if (currentMillis - lastBlinkTime >= 50) {
        lastBlinkTime = currentMillis;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState);
      }

    } else if (voltage < LOW_VOLTAGE_THRESHOLD) {
      Serial.println("ALERT: VOLTAGE LOW!");
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > ALERT_INTERVAL) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "⚠️ Battery Low! Voltage: " + String(voltage, 2) + "V";
            Serial.println("Sending Telegram Alert...");
            if (bot.sendMessage(CHAT_ID, message, "")) {
              Serial.println("Telegram Alert Sent!");
              lastAlertTime = currentMillis;
            } else {
              Serial.println("Failed to send Telegram Alert");
            }
        }
      }

      // Fast Blink for Alarm State
      if (currentMillis - lastBlinkTime >= 100) {
        lastBlinkTime = currentMillis;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState);
      }
      
    } else {
      // Normal State Heartbeat (Slow blink)
      // On for 50ms, Off for 1950ms
      if (ledState == LOW) { // If currently ON
          if (currentMillis - lastBlinkTime >= 50) {
            ledState = HIGH; // Turn OFF
            digitalWrite(LED_BUILTIN, ledState);
            lastBlinkTime = currentMillis;
          }
      } else { // If currently OFF
          if (currentMillis - lastBlinkTime >= 2000) {
             ledState = LOW; // Turn ON
             digitalWrite(LED_BUILTIN, ledState);
             lastBlinkTime = currentMillis;
          }
      }
    }
  }
}
