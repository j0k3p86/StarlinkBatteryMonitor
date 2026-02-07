# Starlink Battery Monitor

This project is an ESP8266-based battery monitoring system designed to keep track of a battery bank (e.g., for a Starlink setup). It monitors voltage levels, sends periodic reports via Telegram, and triggers critical alerts if the voltage drops too low.

## 🔋 Features

-   **Voltage Monitoring**: Continuously reads battery voltage using a voltage divider.
-   **Battery Chemistry Profiles**: Supports **Lead-Acid** (Default), **LiFePO4** (4S), and **Li-ion** (3S) discharge curves.
-   **Telegram Integration**:
    -   **Interactive Bot**: Respond to commands like `/status`, `/calibrate`, and configuration commands.
    -   **Alerts**: Sends **CRITICAL** and **LOW BATTERY** alerts immediately.
-   **Startup Report**: Sends a full status report immediately on boot.
    -   **Periodic Reports**: Configurable hourly/daily status reports.
    -   **Dynamic Configuration**: Adjust thresholds and intervals on the fly.
    -   **Config Validation**: Warns on startup if configuration settings are missing.
-   **MQTT Integration**: Publishes voltage, percentage, and status to an MQTT broker (e.g., Home Assistant).
-   **System Reliability**:
    -   **Hysteresis**: Prevents alert flooding.
    -   **Watchdog Timer (WDT)**: Auto-resets if the system hangs.
    -   **LittleFS Storage**: Settings persist across reboots.

## 🤖 Telegram Commands

-   `/status` : Get current voltage, percentage, signal strength, and config.
-   `/calibrate <voltage>` : Calibrate the sensor (e.g., `/calibrate 12.8`).
-   `/setlow <percent>` : Set Low Battery Warning threshold (Default 20%).
-   `/setcritical <percent>` : Set Critical Battery Alert threshold (Default 10%).
-   `/setalert <minutes>` : Set Critical Alert Interval (Default 10 min).
-   `/setreport <minutes>` : Set Periodic Report Interval (Default 60 min).
-   `/setchemistry <type>` : Set Battery Chemistry (`lead`, `lifepo4`, `lion`).
-   `/setwifi2 <ssid> <pass>` : Set Secondary/Backup WiFi Network.
-   `/mute` / `/unmute` : Disable/Enable periodic reports.
-   `/reset` : **Clear WiFi/Settings** and reboot to AP mode.
-   `/debug <on/off>` : Toggle verbose Serial logging.
-   `/crashlog` : View last reset reason and crash details.
-   `/clearlog` : Delete the crash log file.
-   `/help` : List all commands.

## 📡 MQTT Integration
The device publishes to the following topics:
-   `starlink/status` : `online`
-   `starlink/voltage` : Current Voltage (e.g. `12.50`)
-   `starlink/percentage` : Estimated Capacity % (e.g. `85`)

Configure the MQTT Broker (Server, Port, User, Password) via the WiFiManager portal.

## 🛠 Hardware Requirements

-   **Microcontroller**: ESP8266 (e.g., Wemos D1 Mini).
-   **Voltage Sensor**: Voltage Divider circuit.
    -   **R1**: 100kΩ (to Battery +)
    -   **R2**: 22kΩ (to Ground)
    -   *Connect center point to A0.*

## ⚙️ Configuration

**No coding required for secrets!**

1.  **Flash the code** to your ESP8266.
2.  **First Boot**: The device creates a WiFi Hotspot named **`StarlinkMonitor-Setup`**.
3.  **Connect** to this network.
4.  **Open Browser**: Go to `192.168.4.1`.
5.  **Configure**:
    -   WiFi Network & Password.
    -   Telegram Bot Token & Chat ID.
    -   **MQTT Settings (Optional)**.
    -   **Secondary WiFi**: Use `/setwifi2` command after setup.
    -   **Security**: Default passwords (`starlink`) for `admin_password` (WiFi Portal) and `ota_password` (Code Upload).

6.  **Save**: The device reboots and connects.

## 🌐 Web Dashboard & Home Assistant

### Local Dashboard
Access the device at `http://<device-ip>/`.
-   **Live Data**: Voltage, %, Signal, Uptime, Boot Count, Cycles.
-   **Dark Mode UI**.

### Home Assistant (Auto-Discovery)
-   No YAML required! 
-   Just configure MQTT in the portal, and the device will automatically appear in Home Assistant.
-   **Sensors**: Voltage, Battery Level, Signal Strength.

## 🔒 Security
-   **WiFi Setup Portal**: Protected by `admin_password` (Default: `starlink`).
-   **OTA Updates**: Protected by `ota_password` (Default: `starlink`).

## 🔋 Advanced Logic
-   **Cycle Tracking**: Counts "Cycles" (Discharge < 30% -> Charge > 95%).
-   **Time-to-Empty (TTE)**: Estimates remaining runtime based on trend.
6.  **Save**: The device reboots and connects.

To reset credentials, send `/reset` to the bot.

## 📦 Libraries Required

Install these using the Arduino Library Manager:
-   `ESP8266WiFi`
-   `UniversalTelegramBot` (by Brian Lough)
-   `ArduinoJson`
-   `WiFiManager` (by tzapu)
-   `PubSubClient` (by Nick O'Leary) **[NEW]**
-   `ArduinoOTA`, `EEPROM`, `FS` (Built-in)

## 🐛 Troubleshooting

-   **Wrong Voltage?** Use a multimeter to measure the battery, then send `/calibrate 12.x`.
-   **No Telegram messages?** Check your Bot Token and Chat ID.
-   **Dashboard Loading Slowly?** The device prioritizes Telegram checks. If the dashboard times out, wait a few seconds and try again. The refresh rate is set to 5 seconds to reduce load.
