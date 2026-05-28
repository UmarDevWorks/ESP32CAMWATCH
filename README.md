
# ESP32-CAM Watch

A multifunctional ESP32-CAM based smart watch system combining embedded hardware, IoT, and a web dashboard.

---

## Overview

This project turns an ESP32-CAM into a compact wearable-style system with:

- OLED interface for local control
- Camera capture with SD card storage
- WiFi connectivity with web dashboard
- Weather data caching via API
- Stopwatch, flashlight, and brightness control
- Power and storage monitoring

It is designed as a learning-focused embedded systems project combining hardware control and full-stack IoT concepts.

---

## Features

### Hardware Features
- ESP32-CAM based system
- SSD1306 OLED display (I2C)
- Button-based navigation (UP / DOWN / SELECT)
- Flash LED control (torch mode)
- SD card support for image storage
- Battery voltage monitoring (ADC)

### Software Features
- Image capture and local SD storage
- Menu-based UI system
- Stopwatch with pause/resume
- Adjustable OLED brightness levels
- Deep sleep mode support
- WiFi setup via access point portal
- Web dashboard for remote control

### IoT Features
- Web server hosted on ESP32
- Photo gallery access via browser
- Real-time device status dashboard
- Weather API integration (OpenWeatherMap)
- JSON caching to SD card

---

## Web Dashboard

When connected to WiFi, the device hosts a local dashboard:

- Live device status
- Battery percentage
- Latest captured image preview
- Weather summary
- WiFi configuration panel
- Photo gallery access

---

## Hardware Wiring (Summary)

| Component | GPIO |
|------------|------|
| OLED SDA   | 3    |
| OLED SCL   | 1    |
| Button UP   | 13   |
| Button DOWN | 14   |
| Button SEL  | 15   |
| Flash LED   | 4    |
| Battery ADC | 16   |

---

## Setup Instructions

### 1. Install Libraries
Install these via Arduino Library Manager:

- WiFi
- WebServer
- Preferences
- Adafruit GFX
- Adafruit SSD1306
- ArduinoJson
- SD_MMC
- esp_camera

---

### 2. Configure Camera Board
Select:
```

AI Thinker ESP32-CAM

````

---

### 3. Upload Code
- Connect ESP32-CAM via FTDI
- Set upload speed to 115200 or 921600
- Upload sketch

---

### 4. First Boot
- Device starts in AP mode if no WiFi saved
- Connect to:
  - SSID: ESP32-Watch
  - Password: 12345678
- Open browser:
  - http://192.168.4.1

---

### 5. Add WiFi
Use the web interface to save SSID and password. Device will reboot and connect automatically.

---

## Weather API Setup

To enable weather features:

1. Get API key from OpenWeatherMap
2. Replace in code:

```cpp
const char *WEATHER_API_KEY = "YOUR_API_KEY_HERE";
````

3. Set your location:

```cpp
q=YOUR_CITY, COUNTRY_CODE
```





