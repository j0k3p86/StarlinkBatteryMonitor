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
#include <PubSubClient.h>
// #include "secrets.h" // Deprecated


// Forward Declarations
float readBatteryVoltageBlocking();
void sendDiscoveryMessage();

extern "C" {
#include "user_interface.h"
}

// --- Configuration ---
unsigned long alertInterval = 600000; // Default 10 Minutes (ms)
unsigned long reportInterval = 3600000; // Default 1 Hour (ms)
const unsigned long VOLTAGE_READ_INTERVAL = 2000; // Read voltage every 2 seconds

const int ANALOG_PIN = A0;
const float R1 = 100000.0; // 100k Ohms
const float R2 = 22000.0;  // 22k Ohms
// Reference voltage of D1 Mini ADC input max (Adjust if needed, typically 3.2V or 3.3V)
const float V_REF = 3.3;   
// Thresholds (Default, but changeable via config)
// Thresholds (Default, but changeable via config)
int lowBatPercent = 20;
int criticalBatPercent = 10;
const int HYSTERESIS_PERCENT = 5; // 5% buffer to prevent alert flipping
int batteryChemistry = 0; // 0=Lead-Acid, 1=LiFePO4 (4S), 2=Li-ion (3S)

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
char mqtt_server[40] = "";
char mqtt_port[6] = "1883";
char mqtt_user[20] = "";
char mqtt_pass[40] = "";
char admin_password[20] = "starlink"; // Default password
char ota_password[20] = "starlink";   // Default password
char wifi_ssid2[32] = "";
char wifi_pass2[64] = "";

// Runtime Primary WiFi (Captured from WiFiManager)
String primary_ssid = "";
String primary_pass = "";

// Variable to track if we should save config
bool shouldSaveConfig = false;
bool configMissing = false; // Track if config was incomplete on load
String missingKeys = ""; // List of missing config keys

WiFiClientSecure botClient;
UniversalTelegramBot bot("", botClient); // Token set in setup

WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
ESP8266WebServer server(80);

// Timing variables
unsigned long lastAlertTime = 0;
unsigned long lastReportTime = 0;
unsigned long lastVoltageReadTime = 0;
unsigned long lastBlinkTime = 0;
bool ledState = HIGH; // Start OFF (High is off for built-in LED usually)
bool reportsEnabled = true; // Control periodic reports
bool debugMode = false; // Verbose Serial logging
bool startupReportSent = false; // Flag to send startup report in loop
long bootCount = 0; // Track number of reboots
long batteryCycles = 0; // Track full charge/discharge cycles
bool midCycle = false; // Flag for cycle tracking

// Advanced Logic State
float avgDischargeRate = 0.0; // % per hour
float tteHours = -1.0; // -1 means unknown/charging
unsigned long lastTTEUpdate = 0;
int lastTTEPercent = -1;

// Bot Timing
int botRequestDelay = 5000;
unsigned long lastTimeBotRan = 0;

// Alert States for Hysteresis
bool lowVoltageActive = false;
bool criticalVoltageActive = false;

// Web Server Activity Tracking
unsigned long lastWebRequestTime = 0;
const unsigned long WEB_ACTIVE_TIMEOUT = 10000; // 10s timeout for "Active" state
const int FAST_BOT_DELAY = 1000;
const int SLOW_BOT_DELAY = 5000;
  
int getBatteryPercentage(float voltage) {
  if (batteryChemistry == 1) { // LiFePO4 (4S)
     // 4S LiFePO4: 100% ~13.4V, 0% ~10.0V (steep drop off)
     if (voltage >= 13.4) return 100;
     if (voltage >= 13.3) return 90;
     if (voltage >= 13.2) return 80;
     if (voltage >= 13.1) return 70; // very flat curve here
     if (voltage >= 13.0) return 60;
     if (voltage >= 12.9) return 50;
     if (voltage >= 12.8) return 40;
     if (voltage >= 12.5) return 30; // knee starts
     if (voltage >= 12.0) return 20; 
     if (voltage >= 10.0) return 10;
     return 0;
  } else if (batteryChemistry == 2) { // Li-ion (3S)
     // 3S Li-ion: 100% 12.6V, 0% 9.0V
     if (voltage >= 12.6) return 100;
     if (voltage >= 12.3) return 90;
     if (voltage >= 12.0) return 80;
     if (voltage >= 11.7) return 70;
     if (voltage >= 11.4) return 60;
     if (voltage >= 11.1) return 50; // Nominal 3.7*3
     if (voltage >= 10.8) return 40;
     if (voltage >= 10.5) return 30;
     if (voltage >= 9.9) return 20;
     if (voltage >= 9.3) return 10;
     return 0;
  } else { // Lead-Acid (Default)
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
  msg += "IP: " + WiFi.localIP().toString() + "\n";
  msg += "Signal: " + String(rssi) + " dBm\n";
  msg += "Uptime: " + getUptime() + "\n";
  msg += "Boot Count: " + String(bootCount) + "\n";
  msg += "Cycles: " + String(batteryCycles) + "\n";
  
  if (tteHours > 0) msg += "Est. Time: " + String(tteHours, 1) + "h\n";
  else if (tteHours == -2) msg += "Est. Time: Charging\n";
  else msg += "Est. Time: Calculating...\n";
  
  msg += "------------------\n";
  msg += "Low Thresh: " + String(lowBatPercent) + "%\n";
  msg += "Crit Thresh: " + String(criticalBatPercent) + "%\n";
  String chemStr = "Lead-Acid";
  if (batteryChemistry == 1) chemStr = "LiFePO4";
  if (batteryChemistry == 2) chemStr = "Li-ion";
  msg += "Chemistry: " + chemStr + "\n";
  
  if (configMissing) {
    msg += "\n⚠️ MISSING CONFIG:\n" + missingKeys + "\n";
  }
  
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
          configMissing = false;
          missingKeys = "";

          if (json.containsKey("bot_token")) strcpy(bot_token, json["bot_token"]);
          else { configMissing = true; missingKeys += "- bot_token: Use /reset to configure\n"; }
          
          if (json.containsKey("chat_id")) strcpy(chat_id, json["chat_id"]);
          else { configMissing = true; missingKeys += "- chat_id: Use /reset to configure\n"; }

          if (json.containsKey("mqtt_server")) strcpy(mqtt_server, json["mqtt_server"]);
          if (json.containsKey("mqtt_port")) strcpy(mqtt_port, json["mqtt_port"]);
          if (json.containsKey("mqtt_user")) strcpy(mqtt_user, json["mqtt_user"]);
          if (json.containsKey("mqtt_pass")) strcpy(mqtt_pass, json["mqtt_pass"]);
          
          if (json.containsKey("low_percent")) lowBatPercent = json["low_percent"];
          // else { configMissing = true; missingKeys += "- low_percent: Use /setlow\n"; }
          
          if (json.containsKey("critical_percent")) criticalBatPercent = json["critical_percent"];
          // else { configMissing = true; missingKeys += "- critical_percent: Use /setcritical\n"; }

          if (json.containsKey("alert_minutes")) alertInterval = json["alert_minutes"].as<unsigned long>() * 60000;
          // else { configMissing = true; missingKeys += "- alert_minutes: Use /setalert\n"; }

          if (json.containsKey("report_minutes")) reportInterval = json["report_minutes"].as<unsigned long>() * 60000;
          // else { configMissing = true; missingKeys += "- report_minutes: Use /setreport\n"; }

          if (json.containsKey("chemistry")) batteryChemistry = json["chemistry"];
          // Optional, default to 0 if missing is fine
          
          if (json.containsKey("boot_count")) bootCount = json["boot_count"];

          if (json.containsKey("admin_password")) strcpy(admin_password, json["admin_password"]);
          if (json.containsKey("ota_password")) strcpy(ota_password, json["ota_password"]);
          
          if (json.containsKey("wifi_ssid2")) strcpy(wifi_ssid2, json["wifi_ssid2"]);
          if (json.containsKey("wifi_pass2")) strcpy(wifi_pass2, json["wifi_pass2"]);
          
          if (json.containsKey("battery_cycles")) batteryCycles = json["battery_cycles"];


        } else {
            Serial.println("Failed to parse config.json");
            configMissing = true;
            missingKeys = "Parse Error";
        }
      }
    } else {
      Serial.println("Config file does not exist");
      configMissing = true;
      missingKeys = "No Config File";
    }
  } else {
    Serial.println("Failed to mount FS");
    configMissing = true;
    missingKeys = "FS Mount Fail";
  }
}

void saveConfig() {
  DynamicJsonDocument json(1024);
  json["bot_token"] = bot_token;
  json["chat_id"] = chat_id;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_user"] = mqtt_user;
  json["mqtt_pass"] = mqtt_pass;
  json["low_percent"] = lowBatPercent;
  json["critical_percent"] = criticalBatPercent;
  json["alert_minutes"] = alertInterval / 60000;
  json["report_minutes"] = reportInterval / 60000;
  json["chemistry"] = batteryChemistry;
  json["boot_count"] = bootCount;
  json["battery_cycles"] = batteryCycles;
  json["admin_password"] = admin_password;
  json["ota_password"] = ota_password;
  json["wifi_ssid2"] = wifi_ssid2;
  json["wifi_pass2"] = wifi_pass2;

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
  }
  serializeJson(json, configFile);
  configFile.close();
}



// --- Crash Logger ---
void saveCrashLog() {
  rst_info *resetInfo = ESP.getResetInfoPtr();
  if (!resetInfo) return;

  // Only log if it's a crash (WDT or Exception)
  // 1=Hardware WDT, 2=Exception, 3=Software WDT
  if (resetInfo->reason == REASON_WDT_RST || 
      resetInfo->reason == REASON_EXCEPTION_RST || 
      resetInfo->reason == REASON_SOFT_WDT_RST) {
        
    if (LittleFS.begin()) {
      File logFile = LittleFS.open("/crash.log", "a"); // Append mode
      if (logFile) {
        if (logFile.size() > 2000) {
           // Rotate or clear if too big? For now, just simplistic clearance
           logFile.close();
           LittleFS.remove("/crash.log");
           logFile = LittleFS.open("/crash.log", "w");
        }
        
        logFile.println("--- CRASH DETECTED ---");
        logFile.print("Reason: "); logFile.println(ESP.getResetReason());
        logFile.print("Exception Cause: "); logFile.println(resetInfo->exccause);
        logFile.print("EPC1: 0x"); logFile.println(resetInfo->epc1, HEX);
        logFile.print("EPC2: 0x"); logFile.println(resetInfo->epc2, HEX);
        logFile.print("EPC3: 0x"); logFile.println(resetInfo->epc3, HEX);
        logFile.print("ExcVaddr: 0x"); logFile.println(resetInfo->excvaddr, HEX);
        logFile.println("----------------------");
        logFile.close();
        Serial.println("Crash logged to /crash.log");
      }
    }
  }
}



// --- Web Dashboard ---

const char MAIN_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Starlink Monitor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { background-color: #121212; color: #e0e0e0; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #bb86fc; margin-bottom: 5px; }
    .card { background-color: #1e1e1e; border-radius: 15px; padding: 20px; margin: 15px auto; max-width: 400px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
    .value { font-size: 48px; font-weight: bold; color: #03dac6; }
    .label { font-size: 14px; color: #b0b0b0; text-transform: uppercase; letter-spacing: 1px; }
    .unit { font-size: 20px; color: #b0b0b0; }
    .row { display: flex; justify-content: space-around; flex-wrap: wrap; }
    .small-card { background-color: #2c2c2c; border-radius: 10px; padding: 15px; margin: 5px; min-width: 100px; flex: 1; }
    .small-val { font-size: 24px; font-weight: bold; color: #cf6679; }
    .batt-container { position: relative; margin: 20px auto; width: 200px; height: 100px; }
    /* Simple CSS Gauge or just text for now */
    footer { margin-top: 30px; font-size: 12px; color: #555; }
  </style>
  <script>
    function refreshData() {
      fetch('/api/status').then(response => response.json()).then(data => {
        document.getElementById('volts').innerText = data.voltage.toFixed(2);
        document.getElementById('percent').innerText = data.percent;
        document.getElementById('rssi').innerText = data.rssi;
        document.getElementById('uptime').innerText = data.uptime;
        document.getElementById('boot').innerText = data.boot_count;
        
        // Dynamic Color for Percent
        let p = data.percent;
        let c = document.getElementById('percent');
        if(p <= 10) c.style.color = '#cf6679'; // Red
        else if(p <= 20) c.style.color = '#ffb74d'; // Orange
        else c.style.color = '#03dac6'; // Teal
      });
    }
    setInterval(refreshData, 5000); // Update every 5s
    window.onload = refreshData;
  </script>
</head>
<body>
  <h1>Starlink Battery</h1>
  <div class="card">
    <div class="label">Battery Level</div>
    <div class="value"><span id="percent">--</span><span class="unit">%</span></div>
  </div>

  <div class="row" style="max-width: 440px; margin: 0 auto;">
    <div class="small-card">
      <div class="label">Voltage</div>
      <div class="small-val"><span id="volts">--</span><span class="unit">V</span></div>
    </div>
    <div class="small-card">
      <div class="label">Signal</div>
      <div class="small-val"><span id="rssi">--</span><span class="unit">dBm</span></div>
    </div>
  </div>
  
  <div class="card" style="font-size: 14px;">
    <div class="label">System Stats</div>
    <p>Uptime: <span id="uptime" style="color: #fff">--</span></p>
    <p>Boot Count: <span id="boot" style="color: #fff">--</span></p>
  </div>
  
  <footer>
    ESP8266 Starlink Monitor<br>
    <a href="/api/status" style="color: #555;">JSON API</a>
  </footer>
</body>
</html>
)=====";

void handleRoot() {
  lastWebRequestTime = millis();
  server.send_P(200, "text/html", MAIN_PAGE);
}

void handleAPI() {
  lastWebRequestTime = millis();
  
  char json[512];
  snprintf(json, sizeof(json), 
    "{\"voltage\":%.2f,\"percent\":%d,\"rssi\":%ld,\"uptime\":\"%s\",\"boot_count\":%ld,\"cycles\":%ld,\"tte\":%.1f,\"heap\":%u,\"low_thresh\":%d,\"crit_thresh\":%d}",
    currentVoltage,
    getBatteryPercentage(currentVoltage),
    WiFi.RSSI(),
    getUptime().c_str(),
    bootCount,
    batteryCycles,
    tteHours,
    ESP.getFreeHeap(),
    lowBatPercent,
    criticalBatPercent
  );
  
  server.send(200, "application/json", json);
}

// --- Command Handlers ---

void cmdStatus(String chat_id) {
  bot.sendMessage(chat_id, getStatusMessage(), "Markdown");
}

void cmdCalibrate(String chat_id, String arg) {
  if (arg.length() == 0) {
    bot.sendMessage(chat_id, "❌ Usage: /calibrate <voltage>", "");
    return;
  }
  float trueVoltage = arg.toFloat();
  if (trueVoltage > 5.0 && trueVoltage < 25.0) {
    if (currentVoltage > 1.0) {
         calibrationFactor = (trueVoltage * calibrationFactor) / currentVoltage;
         saveCalibration();
         currentVoltage = trueVoltage; 
         sampleCount = 0;
         bot.sendMessage(chat_id, "✅ Calibration updated. Factor: " + String(calibrationFactor, 4), "");
         bot.sendMessage(chat_id, "New Voltage: " + String(trueVoltage, 2) + "V", "");
    } else {
         bot.sendMessage(chat_id, "❌ Voltage too low to calibrate.", "");
    }
  } else {
    bot.sendMessage(chat_id, "❌ Invalid voltage. Range: 5-25V", "");
  }
}

void cmdReset(String chat_id) {
  bot.sendMessage(chat_id, "🔄 Resetting settings and restarting... Connect to 'StarlinkMonitor-Setup' AP.", "");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void cmdCrashLog(String chat_id) {
  if (!LittleFS.exists("/crash.log")) {
    bot.sendMessage(chat_id, "✅ No crashes recorded.", "");
    return;
  }
  
  File logFile = LittleFS.open("/crash.log", "r");
  if (logFile) {
    String logContent = "🐞 **Crash Log**\n```\n";
    while (logFile.available()) {
      logContent += (char)logFile.read();
      // Split messages if too long for Telegram (limit 4096)
      if (logContent.length() > 3000) {
         logContent += "\n```";
         bot.sendMessage(chat_id, logContent, "Markdown");
         logContent = "```\n(continued)\n";
      }
    }
    logContent += "```";
    logFile.close();
    bot.sendMessage(chat_id, logContent, "Markdown");
  } else {
    bot.sendMessage(chat_id, "❌ Failed to open log file.", "");
  }
}

void cmdClearLog(String chat_id) {
  if (LittleFS.remove("/crash.log")) {
    bot.sendMessage(chat_id, "🗑️ Crash log cleared.", "");
  } else {
    bot.sendMessage(chat_id, "❌ Failed to clear log (or empty).", "");
  }
}

void cmdSetLow(String chat_id, String arg) {
  int newVal = arg.toInt();
  if (newVal > criticalBatPercent && newVal <= 100) {
    lowBatPercent = newVal;
    saveConfig();
    bot.sendMessage(chat_id, "✅ Low Battery set to: " + String(lowBatPercent) + "%", "");
  } else {
     bot.sendMessage(chat_id, "❌ Invalid. Must be > Critical (" + String(criticalBatPercent) + "%) and <= 100", "");
  }
}

void cmdSetCritical(String chat_id, String arg) {
  int newVal = arg.toInt();
  if (newVal >= 0 && newVal < lowBatPercent) {
    criticalBatPercent = newVal;
    saveConfig();
    bot.sendMessage(chat_id, "✅ Critical Battery set to: " + String(criticalBatPercent) + "%", "");
  } else {
     bot.sendMessage(chat_id, "❌ Invalid. Must be < Low (" + String(lowBatPercent) + "%)", "");
  }
}

void cmdSetAlert(String chat_id, String arg) {
  unsigned long minutes = arg.toInt();
  if (minutes >= 1 && minutes <= 1440) {
    alertInterval = minutes * 60000;
    saveConfig();
    bot.sendMessage(chat_id, "✅ Alert Interval set to: " + String(minutes) + " min", "");
  } else {
     bot.sendMessage(chat_id, "❌ Invalid. Range: 1-1440 min", "");
  }
}

void cmdSetReport(String chat_id, String arg) {
  unsigned long minutes = arg.toInt();
  if (minutes >= 5 && minutes <= 10080) {
    reportInterval = minutes * 60000;
    saveConfig();
    bot.sendMessage(chat_id, "✅ Report Interval set to: " + String(minutes) + " min", "");
  } else {
     bot.sendMessage(chat_id, "❌ Invalid. Range: 5-10080 min", "");
  }
}

void cmdSetChemistry(String chat_id, String arg) {
  String type = arg;
  type.toLowerCase();
  int oldChem = batteryChemistry;
  
  if (type == "lead" || type == "leadacid") batteryChemistry = 0;
  else if (type == "lifepo4" || type == "lfp") batteryChemistry = 1;
  else if (type == "lion" || type == "liion" || type == "li-ion") batteryChemistry = 2;
  else {
    bot.sendMessage(chat_id, "❌ Usage: /setchemistry <lead|lifepo4|lion>", "");
    return;
  }
  
  if (oldChem != batteryChemistry) {
    saveConfig();
    String name = (batteryChemistry == 1) ? "LiFePO4" : (batteryChemistry == 2) ? "Li-ion" : "Lead-Acid";
    bot.sendMessage(chat_id, "✅ Chemistry set to: " + name, "");
  } else {
    bot.sendMessage(chat_id, "ℹ️ Chemistry already set to type.", "");
  }
}

void cmdSetWifi2(String chat_id, String arg) {
  // Format: "SSID Password"
  int spaceIndex = arg.indexOf(' ');
  if (spaceIndex == -1) {
    bot.sendMessage(chat_id, "❌ Usage: /setwifi2 <SSID> <Password>", "");
    return;
  }
  String s2 = arg.substring(0, spaceIndex);
  String p2 = arg.substring(spaceIndex + 1);
  
  if (s2.length() > 31 || p2.length() > 63) {
    bot.sendMessage(chat_id, "❌ SSID/Pass too long.", "");
    return;
  }
  
  strcpy(wifi_ssid2, s2.c_str());
  strcpy(wifi_pass2, p2.c_str());
  saveConfig();
  bot.sendMessage(chat_id, "✅ Secondary WiFi set to: " + s2, "");
  bot.sendMessage(chat_id, "System will try this if Primary WiFi fails.", "");
}

void cmdDebug(String chat_id, String arg) {
  if (arg == "on") {
    debugMode = true;
    bot.sendMessage(chat_id, "🐞 Debug Mode ON. Check Serial Monitor.", "");
    Serial.println(F("DEBUG: Enabled"));
  } else if (arg == "off") {
    debugMode = false;
    bot.sendMessage(chat_id, "🚫 Debug Mode OFF.", "");
    Serial.println(F("DEBUG: Disabled"));
  } else {
    bot.sendMessage(chat_id, "❌ Usage: /debug <on/off>", "");
  }
}

void cmdMute(String chat_id) {
  reportsEnabled = false;
  bot.sendMessage(chat_id, "🔕 Reports muted.", "");
}

void cmdUnmute(String chat_id) {
  reportsEnabled = true;
  bot.sendMessage(chat_id, "🔔 Reports unmuted.", "");
}

void cmdHelp(String chat_id, String from_name) {
  String welcome = "Welcome, " + from_name + ".\n";
  welcome += "Battery Monitor Bot Commands:\n\n";
  welcome += "/status : Get current status\n";
  welcome += "/mute / /unmute : Control reports\n";
  welcome += "/calibrate <v> : Calibrate sensor\n";
  welcome += "/reset : Reset WiFi & Reboot\n";
  welcome += "/crashlog : View crash history\n";
  welcome += "/clearlog : Clear crash history\n";
  welcome += "/debug <on/off> : Toggle verbose logging\n";
  welcome += "/setlow <%> : Set Low Threshold\n";
  welcome += "/setcritical <%> : Set Critical Threshold\n";
  welcome += "/setalert <m> : Set Alert Interval (min)\n";
  welcome += "/setreport <m> : Set Report Interval (min)\n";
  welcome += "/setchemistry <type> : Set Battery Type (lead, lifepo4, lion)\n";
  welcome += "/setwifi2 <ssid> <pass> : Set Backup WiFi\n\n";
  welcome += "Current Settings:\n";
  welcome += "Low: " + String(lowBatPercent) + "%\n";
  welcome += "Critical: " + String(criticalBatPercent) + "%\n";
  welcome += "Alert Interval: " + String(alertInterval / 60000) + " min\n";
  welcome += "Report Interval: " + String(reportInterval / 60000) + " min\n";
  String chemStr = "Lead-Acid";
  if (batteryChemistry == 1) chemStr = "LiFePO4";
  if (batteryChemistry == 2) chemStr = "Li-ion";
  welcome += "Chemistry: " + chemStr + "\n";
  bot.sendMessage(chat_id, welcome, "Markdown");
}

// --- MQTT Discovery ---
void sendDiscoveryMessage() {
  String device = "\"dev\":{\"ids\":[\"starlink_batt_mon\"],\"name\":\"Starlink Battery Monitor\",\"mdl\":\"ESP8266\",\"mf\":\"Homebrew\"}";
  
  // Voltage
  String p1 = "{\"name\":\"Starlink Voltage\",\"uniq_id\":\"starlink_volts\",\"dev_cla\":\"voltage\",\"unit_of_meas\":\"V\",\"stat_t\":\"starlink/voltage\"," + device + "}";
  mqttClient.publish("homeassistant/sensor/starlink_battery/voltage/config", p1.c_str(), true);
  
  // Percentage
  String p2 = "{\"name\":\"Starlink Battery Level\",\"uniq_id\":\"starlink_percent\",\"dev_cla\":\"battery\",\"unit_of_meas\":\"%\",\"stat_t\":\"starlink/percentage\"," + device + "}";
  mqttClient.publish("homeassistant/sensor/starlink_battery/percent/config", p2.c_str(), true);
  
  Serial.println(F("MQTT Discovery Sent"));
}

void handleCommand(String text, String from_name, String chat_id) {
  String command = text;
  String arg = "";
  
  int spaceIndex = text.indexOf(' ');
  if (spaceIndex != -1) {
    command = text.substring(0, spaceIndex);
    arg = text.substring(spaceIndex + 1);
  }
  
  // Normalize command to lowercase for robustness? Maybe later.
  
  if (command == "/status") cmdStatus(chat_id);
  else if (command == "/calibrate") cmdCalibrate(chat_id, arg);
  else if (command == "/reset") cmdReset(chat_id);
  else if (command == "/setlow") cmdSetLow(chat_id, arg);
  else if (command == "/setcritical") cmdSetCritical(chat_id, arg);
  else if (command == "/setalert") cmdSetAlert(chat_id, arg);
  else if (command == "/setreport") cmdSetReport(chat_id, arg);
  else if (command == "/setchemistry") cmdSetChemistry(chat_id, arg);
  else if (command == "/setwifi2") cmdSetWifi2(chat_id, arg);
  else if (command == "/mute") cmdMute(chat_id);
  else if (command == "/unmute") cmdUnmute(chat_id);
  else if (command == "/debug") cmdDebug(chat_id, arg);
  else if (command == "/crashlog") cmdCrashLog(chat_id);
  else if (command == "/clearlog") cmdClearLog(chat_id);
  else if (command == "/start" || command == "/help") cmdHelp(chat_id, from_name);
  else bot.sendMessage(chat_id, "❓ Unknown command. Try /help", "");
}

void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages: " + String(numNewMessages));

  for (int i = 0; i < numNewMessages; i++) {
    String msg_chat_id = String(bot.messages[i].chat_id);
    if (msg_chat_id != String(chat_id)) {
      bot.sendMessage(msg_chat_id, "Unauthorized user", "");
      continue;
    }
    
    // Dispatch to handler
    handleCommand(bot.messages[i].text, bot.messages[i].from_name, msg_chat_id);
  }
}


void setup() {
  // CRITICAL: Handle Watchdogs immediately
  ESP.wdtDisable(); // Disable Soft WDT to prevent early triggering
  ESP.wdtFeed();    // Feed Hard WDT immediately

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // Ensure off initially

  Serial.begin(115200);
  
  // Safe Delay instead of blocking delay(3000)
  for (int i = 0; i < 30; i++) {
    delay(100);
    ESP.wdtFeed(); 
  }

  Serial.println(F("\n\n--- BATTERY MONITOR SYSTEM STARTING ---"));
  Serial.print(F("Reset Reason: "));
  Serial.println(ESP.getResetReason());
  Serial.print(F("Boot Version: "));
  Serial.println(ESP.getBootVersion());
  Serial.print(F("SDK Version: "));
  Serial.println(ESP.getSdkVersion());
  
  // ESP.wdtDisable() already called at top

  // Crash Logger
  saveCrashLog();

  // File System & Config
  Serial.println(F("STEP: Loading Config..."));
  ESP.wdtFeed();
  loadConfig();
  
  // increment boot count
  bootCount++;
  saveConfig();
  
  Serial.println("STEP: Config Loaded");
  Serial.print("Free Heap: "); Serial.println(ESP.getFreeHeap());

  // WiFiManager - Scoped to free memory immediately after use
  {
      WiFiManager wm;
      wm.setSaveConfigCallback(saveConfigCallback);
      
      WiFiManagerParameter custom_bot_token("bot_token", "Telegram Bot Token", bot_token, 60);
      WiFiManagerParameter custom_chat_id("chat_id", "Telegram Chat ID", chat_id, 20);
      WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
      WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
      WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, 20);
      WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_pass, 40);
      
      wm.addParameter(&custom_bot_token);
      wm.addParameter(&custom_chat_id);
      wm.addParameter(&custom_mqtt_server);
      wm.addParameter(&custom_mqtt_port);
      wm.addParameter(&custom_mqtt_user);
      wm.addParameter(&custom_mqtt_pass);
      
      Serial.println(F("Connecting to WiFi via WiFiManager..."));
      Serial.println(F("STEP: Starting AutoConnect..."));
      ESP.wdtFeed(); 
      yield();
      
      // Use a unique AP name
      if (!wm.autoConnect("StarlinkMonitor-Setup", admin_password)) {
        Serial.println(F("Failed to connect. Restarting..."));
        
        ESP.restart();
      }
      Serial.println("STEP: AutoConnect Finished");
      ESP.wdtFeed();
      
      Serial.println("\nWiFi Connected!");
      
      // Capture Primary Credentials for Failover
      primary_ssid = WiFi.SSID();
      primary_pass = WiFi.psk();
      
      strcpy(bot_token, custom_bot_token.getValue());
      strcpy(chat_id, custom_chat_id.getValue());
      strcpy(mqtt_server, custom_mqtt_server.getValue());
      strcpy(mqtt_port, custom_mqtt_port.getValue());
      strcpy(mqtt_user, custom_mqtt_user.getValue());
      strcpy(mqtt_pass, custom_mqtt_pass.getValue());

      // Save if needed (inside scope to use wm callback result if any)
      // Actually callback sets global flag, so we can save outside? 
      // No, wm is destroyed, but flag remains. 
      // But we should save here? No, let's keep logic simple.
  } 
  // End of WiFiManager Scope - Memory Freed
  
  Serial.println(F("STEP: VM Scope Ended. Memory Freed."));
  Serial.print(F("Free Heap: ")); Serial.println(ESP.getFreeHeap());
  ESP.wdtFeed();

  if (shouldSaveConfig) {
    saveConfig();
  }

  
  // Update Bot Token
  bot.updateToken(bot_token);

  // Secure Client Setup
  botClient.setInsecure(); // Skip certificate validation for simplicity
  botClient.setBufferSizes(1024, 1024); // Reduce memory usage (Default is 16k+16k!)
  
  if (WiFi.status() == WL_CONNECTED) {
    // MQTT Setup
    if (strlen(mqtt_server) > 0) {
      mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    }
    Serial.println("STEP: Reading Voltage...");
    ESP.wdtFeed();
    float voltage = readBatteryVoltageBlocking();
    Serial.println("STEP: Voltage Read Done");
    currentVoltage = voltage; // Initialize global
    
    // NOTE: Startup messages moved to loop() to prevent WDT reset in setup
  }

  // Web Server Routes (Start AFTER WiFiManager to avoid port 80 conflict)
  
  // Force STA mode to be safe
  WiFi.mode(WIFI_STA); 
  
  server.on("/", handleRoot);
  server.on("/api/status", handleAPI);
  
  server.begin();
  Serial.println(F("STEP: Web Server Started"));

  Serial.print(F("ChatID = "));
  Serial.println(chat_id);

  // OTA Setup
  ArduinoOTA.setHostname("StarlinkBatteryMonitor");
  ArduinoOTA.setPassword(ota_password);
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

  // Enable WDT now that we are connected and ready to loop
  Serial.println("STEP: Setup Complete. Enabling WDT.");
  ESP.wdtEnable(8000); 
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
      if (debugMode) {
          Serial.print("DEBUG: Raw ADC Median: ");
          Serial.println(medianValue);
      }
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

// Non-blocking WiFi Reconnection with Failover
void handleWiFiReconnection() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  static unsigned long lastReconnectAttempt = 0;
  static int reconnectAttempts = 0;
  static bool usingBackupWifi = false;
  
  unsigned long now = millis();
  
  // Try to reconnect every 30 seconds if connection is lost
  if (now - lastReconnectAttempt > 30000) {
    lastReconnectAttempt = now;
    reconnectAttempts++;
    
    Serial.print(F("WiFi Disconnected. Attempt: "));
    Serial.println(reconnectAttempts);

    // If we failed 3 times, and we have a backup configured, try switching
    if (reconnectAttempts >= 3 && strlen(wifi_ssid2) > 0) {
       usingBackupWifi = !usingBackupWifi; // Toggle
       reconnectAttempts = 0; // Reset counter
       
       if (usingBackupWifi) {
          Serial.print(F("Switching to Backup WiFi: "));
          Serial.println(wifi_ssid2);
          WiFi.begin(wifi_ssid2, wifi_pass2);
       } else {
          Serial.print(F("Switching to Primary WiFi: "));
          Serial.println(primary_ssid);
          WiFi.begin(primary_ssid.c_str(), primary_pass.c_str());
       }
    } else {
       // Retry current network
       Serial.println(F("Retrying connection..."));
       WiFi.reconnect(); 
    }
  }
}

void reconnectMQTT() {
  if (strlen(mqtt_server) == 0) return; // No MQTT configured

  // Loop until we're reconnected (try once per loop call actually, we don't want to block too long)
  if (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "StarlinkMonitor-";
    clientId += String(random(0xffff), HEX);
    
    // Attempt to connect
    bool connected = false;
    if (strlen(mqtt_user) > 0) {
      connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    } else {
      connected = mqttClient.connect(clientId.c_str());
    }

    if (connected) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      mqttClient.publish("starlink/status", "online");
      sendDiscoveryMessage();
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again next loop");
    }
  }
}

// --- Advanced Logic ---
void updateAdvancedLogic(int percent) {
  unsigned long currentMillis = millis();
  
  // 1. Cycle Tracking
  // Cycle starts when we drop below 30% (Deep Discharge)
  if (percent <= 30 && !midCycle) {
    midCycle = true;
    // We don't save just for this flag, but we could.
  }
  // Cycle completes when we charge back above 95%
  if (percent >= 95 && midCycle) {
    midCycle = false;
    batteryCycles++;
    saveConfig(); // Persist the new count
    Serial.println("EVENT: Cycle Completed!");
  }

  // 2. Time-to-Empty (TTE)
  // Update every minute to track trend
  if (currentMillis - lastTTEUpdate >= 60000) {
    if (lastTTEPercent != -1) {
       int diff = lastTTEPercent - percent; // Positive means discharging
       
       if (diff > 0) {
          // Discharging
          float ratePerHour = (float)diff * 60.0; // Simplistic projection per hour based on 1 min? No, too noisy.
          // Actually, diff is drop in 1 minute. Slew rate is tricky with integer percents.
          // Better: Use floating point voltage for rate? Or just accept slow tracking.
          // Let's use a VERY slow filter.
       }
       
       // Improved TTE Logic:
       // If percent went down, we are discharging.
       // Rate = (Drop in %) / (Time in Hours)
       // But 1 minute is too short for 1% drop often.
       // Let's just state: If we dropped 1% since last check, how long did it take?
       // This is complicated. Let's stick to a simpler "Trend" approach for now.
       
       // Placeholder: Just mark as "Unknown" if charging, "Calc" if stable.
       // Real TTE requires storing history.
       
       if (currentVoltage > 13.0) tteHours = -2; // Charging
       else tteHours = -1; // Calculating/Unknown
    }
    
    lastTTEPercent = percent;
    lastTTEUpdate = currentMillis;
  }
}

void loop() {
  ESP.wdtFeed(); // Feed the dog!
  ArduinoOTA.handle();
  server.handleClient(); // Handle Web Requests
  handleWiFiReconnection();
  
  unsigned long currentMillis = millis();

  // Startup Reporting (Moved from setup)
  if (!startupReportSent) {
     if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("STEP: Sending Startup Messages..."));
        
        bot.sendMessage(chat_id, "🚀 **System Started!**", "Markdown");
        ESP.wdtFeed(); yield();
        
        if (configMissing) {
           bot.sendMessage(chat_id, "⚠️ **Warning**: Missing Configs: " + missingKeys + "\nDefaults loaded.", "Markdown");
           ESP.wdtFeed(); yield();
        }
        
        bot.sendMessage(chat_id, getStatusMessage(), "Markdown");
        ESP.wdtFeed(); yield();
        
        Serial.println(F("STEP: Startup Messages Sent"));
        startupReportSent = true;
     } else {
        // If not connected yet, we wait for next loop. 
        // But what if it never connects? Logic elsewhere handles reconnect.
     }
  }

  if (debugMode && (currentMillis % 5000 < 50)) {
     // Print a heartbeat every ~5s (dependent on loop speed, simple check)
     Serial.println("DEBUG: Loop Alive. WiFi: " + String(WiFi.status()));
  }

  // Dynamic Telegram Polling
  // If web server was active recently, poll slowly to save CPU/Network for user.
  // Otherwise, poll fast for responsiveness.
  if (currentMillis - lastWebRequestTime < WEB_ACTIVE_TIMEOUT) {
    botRequestDelay = SLOW_BOT_DELAY;
  } else {
    botRequestDelay = FAST_BOT_DELAY;
  }

  // Telegram Bot Handling
  if (startupReportSent && currentMillis > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while(numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = currentMillis;
  }

  // MQTT Handler
  if (strlen(mqtt_server) > 0) {
     if (!mqttClient.connected()) {
         // Try to reconnect occasionally, not every loop to avoid blocking? 
         // PubSubClient `connect` blocks.
         // Let's do it if enough time passed? 
         // Or just call it.
         static unsigned long lastMqttAttempt = 0;
         if (currentMillis - lastMqttAttempt > 5000) {
             lastMqttAttempt = currentMillis;
             reconnectMQTT();
         }
     }
     if (mqttClient.connected()) {
        mqttClient.loop();
     }
  }

  updateVoltageReading();
  float voltage = currentVoltage;

  // Voltage Monitoring Logic
  if (currentMillis - lastVoltageReadTime >= VOLTAGE_READ_INTERVAL) {
    lastVoltageReadTime = currentMillis;
    
    // voltage is already updated by updateVoltageReading()
    
    Serial.print("Voltage: ");
    Serial.print(voltage);
    Serial.print(voltage);
    Serial.println(" V");

    // Publish to MQTT
    if (mqttClient.connected()) {
        char valStr[8];
        dtostrf(voltage, 1, 2, valStr);
        mqttClient.publish("starlink/voltage", valStr);
        String pct = String(getBatteryPercentage(voltage));
        mqttClient.publish("starlink/percentage", pct.c_str());
        
        // Publish Discovery Config only once? Nah, maybe just data.
    }

    // Periodic Reporting
    if (currentMillis - lastReportTime >= reportInterval) {
      if (WiFi.status() == WL_CONNECTED && reportsEnabled) {
        String message = "🔋 Battery Report\nVoltage: " + String(voltage, 2) + "V (" + String(getBatteryPercentage(voltage)) + "%)";
        Serial.println(F("Sending Periodic Telegram Report..."));
        if (bot.sendMessage(chat_id, message, "")) {
           Serial.println(F("Periodic Report Sent!"));
           lastReportTime = currentMillis;
        } else {
           Serial.println(F("Failed to send Periodic Report"));
        }
      }
    }

    int currentPercent = getBatteryPercentage(voltage);
    updateAdvancedLogic(currentPercent);

    if (currentPercent <= criticalBatPercent) {
      Serial.println(F("ALERT: CRITICAL BATTERY!"));
      criticalVoltageActive = true;
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > alertInterval) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "🚨 CRITICAL BATTERY! Level: " + String(currentPercent) + "% (" + String(voltage, 2) + "V)";
            Serial.println(F("Sending Telegram Alert..."));
            if (bot.sendMessage(chat_id, message, "")) {
              Serial.println(F("Critical Telegram Alert Sent!"));
              lastAlertTime = currentMillis;
            } else {
              Serial.println(F("Failed to send Critical Telegram Alert"));
            }
        }
      }
      
      // Very Fast Blink for Critical State
      if (currentMillis - lastBlinkTime >= 50) {
        lastBlinkTime = currentMillis;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState);
      }

    } else if (currentPercent <= lowBatPercent) {
      Serial.println(F("ALERT: BATTERY LOW!"));
      lowVoltageActive = true;
      criticalVoltageActive = false; // Not critical anymore if we are just low
      
      // Send Telegram Alert (Debounced)
      if (currentMillis - lastAlertTime > alertInterval) {
        if (WiFi.status() == WL_CONNECTED) {
            String message = "⚠️ Battery Low! Level: " + String(currentPercent) + "% (" + String(voltage, 2) + "V)";
            Serial.println(F("Sending Telegram Alert..."));
            if (bot.sendMessage(chat_id, message, "")) {
              Serial.println(F("Telegram Alert Sent!"));
              lastAlertTime = currentMillis;
            } else {
              Serial.println(F("Failed to send Telegram Alert"));
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
      
      if (currentPercent >= (lowBatPercent + HYSTERESIS_PERCENT)) {
        lowVoltageActive = false;
        criticalVoltageActive = false;
      }

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
