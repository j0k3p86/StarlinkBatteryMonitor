# Starlink Battery Monitor

This project is an ESP8266-based battery monitoring system designed to keep track of a battery bank (e.g., for a Starlink setup). It monitors voltage levels, sends periodic reports via Telegram, and triggers critical alerts if the voltage drops too low.

## Features

-   **Voltage Monitoring**: Continuously reads battery voltage (non-blocking) using a voltage divider.
-   **Telegram Integration**:
    -   **Interactive Bot**: Respond to commands like `/status` and `/help`.
    -   **Startup Report**: Sends a full status report immediately on boot.
    -   **Periodic Reports**: Sends status reports every **1 hour**.
    -   **Alerts**: sends **CRITICAL** and **LOW VOLTAGE** alerts immediately when thresholds are breached.
-   **System Reliability**:
    -   **Uptime Monitoring**: Reports system uptime.
    -   **Watchdog Timer (WDT)**: Hardware watchdog enabled to auto-reset if the system hangs.
    -   **Dual WiFi Support**: Connects to two different WiFi networks for redundancy.
-   **Visual Feedback**:
    -   **Slow Blink**: Normal operation (Heartbeat).
    -   **Fast Blink**: Low Voltage Warning.
    -   **Very Fast Blink**: Critical Voltage Alert.
-   **Maintenance**:
    -   **OTA Support**: Update firmware wirelessly.
    -   **Calibration**: Calibrate voltage reading via Telegram command.

## Telegram Commands

-   `/status`: Returns current voltage, WiFi SSID, signal strength, and uptime.
-   `/calibrate <voltage>`: Calibrate the sensor to match a multimeter reading (e.g., `/calibrate 12.8`). Saves execution to EEPROM.
-   `/setlow <voltage>`: Set the Low Voltage Warning threshold (default 11.0V).
-   `/setcritical <voltage>`: Set the Critical Voltage Alert threshold (default 10.0V).
-   `/reset`: **Clear WiFi settings** and reboot. Use this to reconfigure the device via the Hotspot.
-   `/mute`: Stop sending periodic hourly reports (alerts still sent).
-   `/unmute`: Resume periodic reports.
-   `/help`: List all available commands.

## Hardware Requirements

-   **Microcontroller**: ESP8266 (e.g., Wemos D1 Mini).
-   **Voltage Sensor**: Voltage Divider circuit.
    -   **R1**: 100kΩ
    -   **R2**: 22kΩ
    -   *Note: Designed for a 12V system, capable of measuring up to ~18V safely with 3.3V ADC.*

## Configuration

**No coding required for secrets!**

1.  **Flash the code** to your ESP8266.
2.  **First Boot**: The device will create a WiFi Hotspot named **`StarlinkMonitor-Setup`**.
3.  **Connect** to this network with your phone or laptop.
4.  **Open Browser**: Go to `192.168.4.1` (it should trigger a captive portal automatically).
5.  **Configure**:
    -   Select your **WiFi Network** and enter password.
    -   Enter your **Telegram Bot Token**.
    -   Enter your **Telegram Chat ID**.
6.  **Save**: The device will save credentials to its internal file system (LittleFS) and reboot.

To reset credentials, send the `/reset` command to the bot. It will clear WiFi settings and reboot into AP mode.

## Adjust Coding Constants (Optional)
If you use different resistor values, check `StarlinkBatteryMonitor.ino`:
-   `R1` / `R2`: Adjust if you use different resistor values.
-   `V_REF`: Adjust if your ESP8266's regulator is slightly different.

## Usage

Flash the code to your ESP8266 using the Arduino IDE. Ensure the following libraries are installed:
-   `ESP8266WiFi`
-   `UniversalTelegramBot`
-   `ArduinoJson`
-   `WiFiManager` (by tzapu)
-   `ArduinoOTA` (Built-in)
-   `EEPROM` (Built-in)
-   `FS` (Built-in)
