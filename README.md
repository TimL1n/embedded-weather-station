# ESP32 Weather Station

A wireless weather station built with an ESP32-C3 microcontroller that collects local sensor data and fetches real-time weather information from the internet.

## Overview

This project demonstrates embedded systems networking by combining:
- **I2C sensor communication** with an SHTC3 temperature/humidity sensor
- **WiFi connectivity** for internet access
- **HTTP client/server architecture** using raw sockets
- **REST API integration** with wttr.in weather service

## How It Works

1. The ESP32 connects to a WiFi network
2. Requests the server's geographic location via HTTP GET
3. Fetches current outdoor temperature from wttr.in for that location
4. Reads the onboard SHTC3 temperature sensor via I2C
5. POSTs both temperatures back to the server
6. Repeats every 5 seconds

## Hardware

- **ESP32-C3-DevKit-RUST-1**
- **SHTC3** - Onboard temperature/humidity sensor (I2C address 0x70)

## Software Architecture

```
┌─────────────┐      HTTP GET /location      ┌─────────────┐
│             │ ◄─────────────────────────── │             │
│   Server    │                              │    ESP32    │
│  (Python)   │      HTTP POST /weather      │             │
│             │ ◄─────────────────────────── │             │
└─────────────┘                              └──────┬──────┘
       │                                            │
       │ IP Geolocation                             │ I2C
       ▼                                            ▼
  ┌─────────┐                                 ┌─────────┐
  │ipinfo.io│                                 │  SHTC3  │
  └─────────┘                                 └─────────┘
                                                    
                    HTTP GET /?format=%t            
              ┌─────────────────────────────►┌─────────┐
              │                              │ wttr.in │
              └──────────────────────────────└─────────┘
```

## Key Technical Details

- **Raw socket HTTP**: Implemented HTTP client using BSD sockets rather than high-level libraries
- **CRC-8 validation**: Verifies sensor data integrity using polynomial 0x31
- **User-Agent spoofing**: Required for wttr.in API compatibility
- **IP-based geolocation**: Server auto-detects location via ipinfo.io API

## Building

Requires ESP-IDF v5.x

```bash
# Set your WiFi credentials and server IP in main.c
idf.py build
idf.py flash monitor
```

## Server

```bash
python server.py              # Auto-detect location
python server.py "New York"   # Manual location
```

## Sample Output

**ESP32:**
```
================================================
           WEATHER STATION REPORT
================================================
  Location:              San Francisco, California
  Outdoor Temperature:   18.00°C
  Sensor Temperature:    25.73°C
================================================
```

**Server:**
```
==================================================
     LAB 7.3 - WEATHER REPORT
==================================================
  Location:            San Francisco, California
  Outdoor Temperature: 18.00°C
  Sensor Temperature:  25.73°C
==================================================
```

## Course

CSE 121 - Embedded Systems, UC Santa Cruz
