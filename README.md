# Starlink Battery Monitor

This project is an ESP8266-based battery monitoring system designed to keep track of a battery bank (e.g., for a Starlink setup). It monitors voltage levels, sends periodic reports via Telegram, and triggers critical alerts if the voltage drops too low.

## Features

-   **Voltage Monitoring**: continuously reads battery voltage using a voltage divider.
-   **Telegram Integration**:
    -   Sends a "System Online" message on startup with current voltage.
    -   Sends periodic status reports every 10 minutes.
    -   Sends **CRITICAL** and **LOW VOLTAGE** alerts immediately when thresholds are breached.
-   **Dual WiFi Support**: configured to connect to two different WiFi networks for redundancy (using `ESP8266WiFiMulti`).
-   **Visual Feedback**:
    -   **Slow Blink**: Normal operation (Heartbeat).
    -   **Fast Blink**: Low Voltage Warning.
    -   **Very Fast Blink**: Critical Voltage Alert.

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
