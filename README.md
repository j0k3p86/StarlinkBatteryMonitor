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

1.  **Clone the repository**.
2.  **Setup Secrets**:
    -   Rename `secrets.h.example` to `secrets.h`.
    -   Open `secrets.h` and enter your credentials:
        -   **WiFi**: SSID and Password for up to 2 networks.
        -   **Telegram**: Bot Token and Chat ID.
3.  **Adjust Coding Constants** (in `StarlinkBatteryMonitor.ino`):
    -   `R1` / `R2`: Adjust if you use different resistor values.
    -   `V_REF`: Adjust if your ESP8266's regulator is slightly different (calibrate with a multimeter).
    -   Thresholds: `LOW_VOLTAGE_THRESHOLD` and `CRITICAL_VOLTAGE_THRESHOLD`.

## Usage

Flash the code to your ESP8266 using the Arduino IDE. Ensure the following libraries are installed:
-   `ESP8266WiFi`
-   `UniversalTelegramBot`
-   `ArduinoJson`
-   `ArduinoOTA` (Built-in)
-   `EEPROM` (Built-in)
