#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <WiFiManager.h>
// #include "secrets.h" // Deprecated


// --- Configuration ---
const unsigned long ALERT_INTERVAL = 600000; // 10 Minutes between alerts (ms)
const unsigned long REPORT_INTERVAL = 3600000; // 1 Hour between reports (ms)
const unsigned long VOLTAGE_READ_INTERVAL = 2000; // Read voltage every 2 seconds

const int ANALOG_PIN = A0;
const float R1 = 100000.0; // 100k Ohms
const float R2 = 22000.0;  // 22k Ohms
// Reference voltage of D1 Mini ADC input max (Adjust if needed, typically 3.2V or 3.3V)
const float V_REF = 3.3;   
// Thresholds (Default, but changeable via config)
float lowVoltageThreshold = 11.0;
float criticalVoltageThreshold = 10.0;
const float HYSTERESIS_THRESHOLD = 0.20; // 0.2V buffer to prevent alert flipping

// Divider Factor: V_in = V_out * (R1 + R2) / R2
const float DIVIDER_RATIO = (R1 + R2) / R2;

// Smoothing
const int NUM_SAMPLES = 20;
const unsigned long SAMPLE_INTERVAL = 10; // 10ms between samples

// Global State
float currentVoltage = 0.0;
float calibrationFactor = 1.0;
unsigned long lastSampleTime = 0;
int voltageSamples[NUM_SAMPLES];
int sampleCount = 0;

// Config Variables
char bot_token[60] = "";
char chat_id[20] = "";

// Variable to track if we should save config
bool shouldSaveConfig = false;

WiFiClientSecure client;
// ESP8266WiFiMulti wifiMulti; // Managed by WiFiManager now
UniversalTelegramBot bot("", client); // Token set in setup

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

// Alert States for Hysteresis
bool lowVoltageActive = false;
bool criticalVoltageActive = false;
  
int getBatteryPercentage(float voltage) {
  // Approximate 12V Lead-Acid discharge curve
  if (voltage >= 12.7) return 100;
  if (voltage >= 12.5) return 90;
  if (voltage >= 12.4) return 80;
  if (voltage >= 12.3) return 70;
  if (voltage >= 12.2) return 60;
  if (voltage >= 12.1) return 50;
  if (voltage >= 12.0) return 40;
  if (voltage >= 11.8) return 30;
  if (voltage >= 11.6) return 20;
  if (voltage >= 11.3) return 10;
  return 0; // < 11.3V is basically empty
}

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
  msg += "Voltage: " + String(currentVoltage, 2) + "V (" + String(getBatteryPercentage(currentVoltage)) + "%)\n";
  
  long rssi = WiFi.RSSI();
  msg += "SSID: " + WiFi.SSID() + "\n";
  msg += "Signal: " + String(rssi) + " dBm\n";
  msg += "Uptime: " + getUptime() + "\n";
  msg += "------------------\n";
  msg += "Low Thresh: " + String(lowVoltageThreshold, 2) + "V\n";
  msg += "Crit Thresh: " + String(criticalVoltageThreshold, 2) + "V\n";
  return msg;
}

void saveCalibration() {
  EEPROM.put(0, calibrationFactor);
  EEPROM.commit();
}

// Config Load/Save
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void loadConfig() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        
        DynamicJsonDocument json(1024);
        DeserializationError error = deserializeJson(json, buf.get());
        if (!error) {
          if (json.containsKey("bot_token")) strcpy(bot_token, json["bot_token"]);
          if (json.containsKey("chat_id")) strcpy(chat_id, json["chat_id"]);
          if (json.containsKey("low_voltage")) lowVoltageThreshold = json["low_voltage"];
          if (json.containsKey("critical_voltage")) criticalVoltageThreshold = json["critical_voltage"];
        }
      }
    }
  } else {
    Serial.println("Failed to mount FS");
  }
}

void saveConfig() {
  DynamicJsonDocument json(1024);
  json["bot_token"] = bot_token;
  json["chat_id"] = chat_id;
  json["low_voltage"] = lowVoltageThreshold;
  json["critical_voltage"] = criticalVoltageThreshold;

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
  }
  serializeJson(json, configFile);
  configFile.close();
}

void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages");
  Serial.println(String(numNewMessages));

  for (int i = 0; i < numNewMessages; i++) {
    String msg_chat_id = String(bot.messages[i].chat_id);
    if (msg_chat_id != String(chat_id)) {
      bot.sendMessage(msg_chat_id, "Unauthorized user", "");
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
             currentVoltage = trueVoltage; 
             // Reset sampling buffer to restart with new factor
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

    if (text == "/reset") {
      bot.sendMessage(chat_id, "🔄 Resetting settings and restarting... Connect to 'StarlinkMonitor-Setup' AP to reconfigure.", "");
      delay(1000);
      WiFiManager wm;
      wm.resetSettings();
      // Also maybe delete config.json? 
      // wm.resetSettings() only clears WiFi. We might want to keep Token/ChatID or clear them too.
      // For now, let's keep it simple: clears WiFi so it launches AP, but keeps old token files unless overwritten.
      ESP.restart();
    }

    if (text.startsWith("/setlow ")) {
      String valueStr = text.substring(8);
      float newVal = valueStr.toFloat();
      if (newVal > criticalVoltageThreshold + 0.1 && newVal < 14.0) {
        lowVoltageThreshold = newVal;
        saveConfig();
        bot.sendMessage(chat_id, "✅ Low Voltage set to: " + String(lowVoltageThreshold, 2) + "V", "");
      } else {
         bot.sendMessage(chat_id, "❌ Invalid value. Must be > Critical (" + String(criticalVoltageThreshold,1) + "V)", "");
      }
    }

    if (text.startsWith("/setcritical ")) {
      String valueStr = text.substring(13);
      float newVal = valueStr.toFloat();
      if (newVal > 8.0 && newVal < lowVoltageThreshold - 0.1) {
        criticalVoltageThreshold = newVal;
        saveConfig();
        bot.sendMessage(chat_id, "✅ Critical Voltage set to: " + String(criticalVoltageThreshold, 2) + "V", "");
      } else {
         bot.sendMessage(chat_id, "❌ Invalid value. Must be < Low (" + String(lowVoltageThreshold,1) + "V)", "");
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
      welcome += "/reset : Clear WiFi settings and reboot to AP Mode\n";
      welcome += "/setlow <v> : Set Low Voltage Threshold\n";
      welcome += "/setcritical <v> : Set Critical Voltage Threshold\n\n";
      welcome += "Current Settings:\n";
      welcome += "Low: " + String(lowVoltageThreshold, 2) + "V\n";
      welcome += "Critical: " + String(criticalVoltageThreshold, 2) + "V\n";
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
  ESP.wdtEnable(8000); 

  // File System & Config
  loadConfig();

  // WiFiManager
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  
  WiFiManagerParameter custom_bot_token("bot_token", "Telegram Bot Token", bot_token, 60);
  WiFiManagerParameter custom_chat_id("chat_id", "Telegram Chat ID", chat_id, 20);
  
  wm.addParameter(&custom_bot_token);
  wm.addParameter(&custom_chat_id);
  
  Serial.println("Connecting to WiFi via WiFiManager...");
  // Use a unique AP name
  if (!wm.autoConnect("StarlinkMonitor-Setup")) {
    Serial.println("Failed to connect. Restarting...");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("\nWiFi Connected!");
  
  strcpy(bot_token, custom_bot_token.getValue());
  strcpy(chat_id, custom_chat_id.getValue());

  if (shouldSaveConfig) {
    saveConfig();
  }

  // Update Bot Token
  bot.updateToken(bot_token);

  // Secure Client Setup
  client.setInsecure(); // Skip certificate validation for simplicity
  
  if (WiFi.status() == WL_CONNECTED) {
    float voltage = readBatteryVoltageBlocking();
    currentVoltage = voltage; // Initialize global
    bot.sendMessage(chat_id, "🚀 **System Started!**", "Markdown");
    bot.sendMessage(chat_id, getStatusMessage(), "Markdown");
  }

  Serial.print("ChatID = ");
  Serial.println(chat_id);

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
  ArduinoOTA.begin();
}

// Simple Bubble Sort for Median Filter
int getMedian(int raw[], int size) {
  int temp;
  // Copy array to avoid modifying the original buffer gathering data (though here we reset anyway)
  // Actually, we can treat the 'raw' array as scratchpad since we reset count after this.
  
  for(int i=0; i < size-1; i++) {
    for(int j=0; j < (size-(i+1)); j++) {
      if(raw[j] > raw[j+1]) {
        temp = raw[j];
        raw[j] = raw[j+1];
        raw[j+1] = temp;
      }
    }
  }
  return raw[size/2];
}

void updateVoltageReading() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = currentMillis;
    voltageSamples[sampleCount] = analogRead(ANALOG_PIN);
    sampleCount++;

    if (sampleCount >= NUM_SAMPLES) {
      int medianValue = getMedian(voltageSamples, NUM_SAMPLES);
      float pinVoltage = (float)medianValue * (V_REF / 1023.0);
      currentVoltage = pinVoltage * DIVIDER_RATIO * calibrationFactor;
      
      // Reset for next batch
      sampleCount = 0;
    }
  }
}

// Helper to get fresh reading initially (blocking)
float readBatteryVoltageBlocking() {
  int localSamples[NUM_SAMPLES];
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    localSamples[i] = analogRead(ANALOG_PIN);
    delay(10);
  }
  
  int medianValue = getMedian(localSamples, NUM_SAMPLES);
  float pinVoltage = (float)medianValue * (V_REF / 1023.0);
  
  // Calculate actual battery voltage
  return pinVoltage * DIVIDER_RATIO * calibrationFactor;
}

void handleWiFiReconnection() {
  if (WiFi.status() != WL_CONNECTED) {
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
        String message = "🔋 Battery Report\nVoltage: " + String(voltage, 2) + "V (" + String(getBatteryPercentage(voltage)) + "%)";
        Serial.println("Sending Periodic Telegram Report...");
        if (bot.sendMessage(chat_id, message, "")) {
           Serial.println("Periodic Report Sent!");
           lastReportTime = currentMillis;
        } else {
           Serial.println("Failed to send Periodic Report");
        }
      }
    }

    if (voltage < criticalVoltageThreshold) {
      Serial.println("ALERT: CRITICAL VOLTAGE!");
      criticalVoltageActive = true;
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > ALERT_INTERVAL) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "🚨 CRITICAL BATTERY LOW! Voltage: " + String(voltage, 2) + "V (" + String(getBatteryPercentage(voltage)) + "%)";
            Serial.println("Sending Telegram Alert...");
            if (bot.sendMessage(chat_id, message, "")) {
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

    } else if (voltage < lowVoltageThreshold) {
      Serial.println("ALERT: VOLTAGE LOW!");
      lowVoltageActive = true;
      criticalVoltageActive = false; // Not critical anymore if we are just low
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > ALERT_INTERVAL) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "⚠️ Battery Low! Voltage: " + String(voltage, 2) + "V (" + String(getBatteryPercentage(voltage)) + "%)";
            Serial.println("Sending Telegram Alert...");
            if (bot.sendMessage(chat_id, message, "")) {
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
      // Logic for Hysteresis Recovery
      // Only consider "Normal" if we are consistently above Threshold + Hysteresis
      
      if (voltage > (lowVoltageThreshold + HYSTERESIS_THRESHOLD)) {
        lowVoltageActive = false;
        criticalVoltageActive = false;
      }

      // Normal State Heartbeat (Slow blink) - Only if no alerts are active!
      // If we are in the "hysteresis zone" (e.g. 11.1V), we keep the warning state implicitly by not running this block?
      // Or we can just run normal blink if voltage > LOW. 
      // User wanted to prevent "Alert Spamming". The Alert Interval (10m) already handles that partially.
      // Hysteresis prevents the case where it dips to 10.99, alerts, rises to 11.01, dips to 10.99, alerts again.
      // With the 10m timer, it wouldn't alert again anyway.
      // But Hysteresis is still good practice.
      
      // Normal State Heartbeat
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
