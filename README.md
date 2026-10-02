# Quill Moon Phase Clock

An ESP32-powered round-display clock showing the current time, date and real-time moon phase on a 240×240 GC9A01 display.

This version uses a **non-blocking architecture** so that Wi-Fi connection, NTP synchronisation and NASA moon-data retrieval do not interrupt the display or user interface.

Moon phase data is obtained from NASA's Dial-a-Moon API and rendered using a 30-frame moon image cycle.

## Credits

This project is based on the **Moon Phase Clock** project by **nishad2m8**:

https://github.com/nishad2m8

The Quill version retains the original LVGL/SquareLine user interface and moon imagery while adding hardware configuration changes and a non-blocking networking architecture for the ESP32.

## Features

- 240×240 round GC9A01 display
- Live local time and date via NTP
- Automatic timezone and daylight-saving handling
- Current moon phase from NASA's Dial-a-Moon API
- 30-frame moon phase image cycle
- Eight named lunar phases
- Three-second moon animation at startup
- Non-blocking Wi-Fi connection and reconnection
- Non-blocking NTP synchronisation
- NASA HTTPS requests performed in a background FreeRTOS task
- Display remains responsive during network operations
- Automatic retry following network or API failure
- LVGL 8.3.11 user interface
- SquareLine Studio generated UI
- PlatformIO build environment

## Hardware

- ESP32 WROOM development board
- GC9A01 240×240 round SPI display

## Wiring

| Display Pin | ESP32 GPIO |
|-------------|------------|
| VCC | 3.3V |
| GND | GND |
| MOSI / SDA | GPIO 23 |
| SCLK / SCL | GPIO 18 |
| CS | GPIO 5 |
| DC | GPIO 2 |
| RST / RES | GPIO 4 |
| BL / BLK | 3.3V |
| MISO | Not connected |

The display uses hardware SPI.

## Software

The project is built using PlatformIO with the Arduino framework.

Main libraries:

- LVGL 8.3.11
- TFT_eSPI 2.5.43
- ArduinoJson 6.x
- ESP32 WiFi
- HTTPClient
- WiFiClientSecure

## Configuration

Local configuration is stored in:

`include/secrets.h`

This file should **not be committed to Git**.

Create it from the supplied `secrets.example.h` and enter your own settings:

```cpp
#ifndef SECRETS_H
#define SECRETS_H

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* TIMEZONE =
    "NZST-12NZDT,M9.5.0,M4.1.0/3";

#endif
