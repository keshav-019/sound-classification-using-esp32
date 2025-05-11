# ESP32 Audio Classification System

![System Architecture Diagram](https://i.imgur.com/JQhG8xO.png)

## Project Overview

This ESP32-based system provides real-time audio classification with baby crying detection capabilities. The device records audio via a PDM microphone, processes the audio using a TensorFlow Lite model, and provides a web interface for control and monitoring.

## Key Components

1. **File Operations** - Handles all SD card/filesystem operations including:
   - Audio file storage/retrieval
   - Directory management
   - File deletion/listing

2. **HTTP Server** - Web interface with these endpoints:
   - `/` - Main dashboard
   - `/record` - Audio recording control
   - `/predict` - Classification results
   - `/files` - Recordings management
   - `/ota` - Firmware updates

3. **Audio Model** - TensorFlow Lite implementation that classifies:
   - Baby crying sounds
   - Background noise
   - Other audio patterns

4. **Audio Recorder** - PDM microphone handling with:
   - 16kHz sampling rate
   - 5-second recording chunks
   - Automatic gain control

5. **WiFi Soft AP** - Wireless access point with:
   - Configurable SSID/password
   - Station mode fallback
   - DNS server integration

## Supported Hardware Targets

| Chip | Status | Notes |
|------|--------|-------|
| ESP32 | ✅ Fully supported | All features available |
| ESP32-S2 | ✅ Fully supported | Better power efficiency |
| ESP32-S3 | ✅ Fully supported | Recommended for new designs |
| ESP32-C3 | ✅ Supported | Reduced RAM may limit recording length |
| ESP32-C6 | ✅ Supported | WiFi 6 compatible |

## Features

- **Audio Classification**:
  - Real-time baby crying detection
  - Background noise filtering
  - Confidence level reporting

- **Web Interface**:
  - Mobile-responsive dashboard
  - Recording controls
  - Prediction results display
  - File management

- **System Management**:
  - Over-the-Air (OTA) updates
  - Configuration portal
  - System monitoring

## Hardware Requirements

- **Required**:
  - ESP32 development board (with PDM interface)
  - Microphone (PDM or I2S)
  - SD card (minimum 1GB, FAT32 formatted)
  - USB cable for programming

- **Recommended**:
  - 10kΩ pull-up resistors for SD card
  - External power supply for stable operation
  - Enclosure for noise reduction

## Installation Guide

### 1. Prerequisites

```bash
# On Linux
sudo apt-get install git wget flex bison gperf python3 python3-pip cmake ninja-build ccache libffi-dev libssl-dev dfu-util

# Initialize ESP-IDF
. $HOME/esp/esp-idf/export.sh