# SpineSense – Wearable Posture Monitoring System

SpineSense is a wearable posture-monitoring prototype designed to encourage better posture during long study and computer sessions.

## Features

- **Posture monitoring:** Uses an MPU6050 accelerometer and gyroscope to detect changes in body tilt.
- **Posture alerts:** Activates a buzzer when an incorrect posture persists beyond a set duration.
- **Touch controls:** Supports temporary snoozing and posture recalibration through a TTP223 touch sensor.
- **Live dashboard:** Hosts a web dashboard over the ESP8266's Wi-Fi access point to display sensor and posture status.
- **Light sensing:** Uses an LDR module to read ambient light levels.

## Hardware Components

- ESP8266 NodeMCU
- MPU6050 motion sensor
- TTP223 touch sensor
- Piezo buzzer
- LDR module
- Jumper wires and USB power supply

## Project Status

This is a working prototype developed for a team hackathon project. It achieved **4th place among 96 teams**.

## Limitations

SpineSense estimates body tilt using motion-sensor readings. It does not directly measure spinal curvature or diagnose posture-related medical conditions.

## Getting Started

1. Install the Arduino IDE and configure ESP8266 board support.
2. Install the libraries required by the firmware.
3. Connect the components according to the project's wiring diagram.
4. Open `SpineSense.ino` in the Arduino IDE.
5. Update the Wi-Fi access-point credentials before uploading.
6. Upload the firmware to the ESP8266 NodeMCU.
7. Connect to the SpineSense Wi-Fi network and open `http://192.168.4.1` in a browser.

## Future Improvements

- Improve posture calibration and repeatability.
- Refine the buzzer alert pattern.
- Explore remote dashboard access and data logging.

**Note:** This project is an educational prototype, not a medical device.
