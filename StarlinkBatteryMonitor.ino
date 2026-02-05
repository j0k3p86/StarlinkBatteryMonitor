#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "secrets.h"

// --- Configuration ---
const unsigned long ALERT_INTERVAL = 3600000; // 1 Hour between alerts (ms)
const unsigned long REPORT_INTERVAL = 600000; // 10 Minutes between reports (ms)
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

WiFiClientSecure client;
ESP8266WiFiMulti wifiMulti;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Timing variables
unsigned long lastAlertTime = 0;
unsigned long lastReportTime = 0;
unsigned long lastVoltageReadTime = 0;
unsigned long lastBlinkTime = 0;
bool ledState = HIGH; // Start OFF (High is off for built-in LED usually)

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // Ensure off initially

  Serial.begin(115200);
  delay(3000); // Allow Serial to settle
  Serial.println("\n\n--- BATTERY MONITOR SYSTEM STARTING ---");

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
    float voltage = readBatteryVoltage();
    bot.sendMessage(CHAT_ID, "🔋 Battery Monitor Online!", "");
    bot.sendMessage(CHAT_ID, "Voltage: " + String(voltage, 2) + "V", "");
  }

  Serial.print("ChatID = ");
  Serial.println(CHAT_ID);
}

float readBatteryVoltage() {
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
  return pinVoltage * DIVIDER_RATIO;
}

void handleWiFiReconnection() {
  if (wifiMulti.run() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
  }
}

void loop() {
  handleWiFiReconnection();
  
  unsigned long currentMillis = millis();

  // Voltage Monitoring Logic
  if (currentMillis - lastVoltageReadTime >= VOLTAGE_READ_INTERVAL) {
    lastVoltageReadTime = currentMillis;
    
    float voltage = readBatteryVoltage();
    
    Serial.print("Voltage: ");
    Serial.print(voltage);
    Serial.println(" V");

    // Periodic Reporting (Every 10 minutes)
    if (currentMillis - lastReportTime >= REPORT_INTERVAL) {
      if (WiFi.status() == WL_CONNECTED) {
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
