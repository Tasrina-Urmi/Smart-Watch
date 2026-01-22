# ESP32 Smartwatch (OLED + RTC + MAX30102 + GPS + BLE)

A feature-rich ESP32 smartwatch project with an OLED UI, multiple watch faces, health tracking (Heart Rate + SpO2), GPS location data, and BLE connectivity to send/receive data (weather, commands, etc).

## ✨ Features

### 🕒 Watch Faces (5 Total)
1. **Digital Watch Face**
   - Time + Date
   - Battery indicator
   - City/Location label

2. **Analog Watch Face**
   - Real-time analog clock display

3. **Health Face**
   - Heart Rate (HR)
   - SpO2
   - Step counter
   - Animated progress ring
   - Reset option
   - Send health stats to phone via BLE

4. **Location + Weather Face**
   - GPS location (lat/lon, speed, satellites)
   - Weather string received from phone over BLE
   - Optional sending of GPS/location data

5. **Menu Face**
   - Alarm
   - Stopwatch
   - Settings
   - Game placeholder

---

## 🧰 Hardware Requirements

- **ESP32 Dev Board**
- **OLED Display** (U8g2 compatible, I2C recommended)
- **RTC Module** (DS3231 supported via RTClib)
- **MAX30102 / MAX30105** (Heart rate + SpO2 sensor)
- **MPU6050** (Steps / motion)
- **GPS Module** (UART)
- **Buttons**
  - UP
  - OK/SELECT
  - BACK
- **Battery Voltage Divider** (for ADC battery read)

---

## 🔌 Pin Configuration (Defaults)

### Buttons
| Button | GPIO |
|--------|------|
| UP     | 32   |
| OK     | 33   |
| BACK   | 25   |

### GPS (UART2)
| GPS Pin | ESP32 GPIO |
|---------|------------|
| GPS TX  | 16 (ESP32 RX2) |
| GPS RX  | 17 (ESP32 TX2) |

### Battery ADC
| Signal | GPIO |
|--------|------|
| Battery ADC | 34 |

> Battery divider ratio default: `2.0`

---

## 📦 Libraries Used

Install these libraries in the Arduino IDE Library Manager (or PlatformIO equivalents):

- `U8g2`
- `RTClib`
- `TinyGPSPlus`
- `Preferences`
- `MAX30105`
- `Adafruit MPU6050`
- `Adafruit Unified Sensor`
- `ESP32 BLE Arduino` (BLEDevice / BLEServer / BLEUtils / BLE2902)

Also required:
- `spo2_algorithm.h` (included with MAX3010x SpO2 implementations)

---

## 📡 BLE Communication

This project exposes a BLE service with multiple characteristics.

### ✅ BLE UUIDs

**Service UUID**
- `8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c01`

**Characteristics**
| Name | UUID | Properties | Format |
|------|------|------------|--------|
| Health | `8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c02` | Notify | `"hr,spo2,steps"` |
| Location | `8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c03` | Notify | `"lat,lon,speed,sats"` |
| Weather | `8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c04` | Read/Write | `"City|TempC|Cond"` |
| Commands | `8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c05` | Write | command strings |

### 🌦 Weather Payload Format
The phone/app should write a string in this format to the Weather characteristic:

```
City|TempC|Cond
```

Example:
```
Dhaka|31|Cloudy
```

---

## ⚙️ Setup Instructions

### 1) Flash the Code
- Open `SmartWatch_BLE_GPS_Fixed.ino` in **Arduino IDE**
- Select:
  - Board: **ESP32 Dev Module**
  - Correct COM port
- Upload

### 2) Wiring
Connect the modules using the default pins shown above.

### 3) BLE Companion App (Optional)
Use:
- **nRF Connect** (Android/iOS)
- Any BLE app that can write to characteristics

You can:
- Write weather data
- Receive health + GPS notifications

---

## 🔋 Battery Reading Notes

Battery voltage uses ADC and a divider ratio:

- `BAT_V_MIN = 3.30V`
- `BAT_V_MAX = 4.20V`
- Divider ratio: `2.0`
- ADC reference: `3.3V`

Make sure your voltage divider is correctly designed so battery voltage stays within ESP32 ADC safe range.

---

## 🛠 Customization

You can easily adjust:

- Button pins
- GPS pins
- Battery calibration (divider ratio, min/max voltage)
- Add new menu options / faces
- Improve step algorithm tuning (MPU6050)

---

## ✅ Roadmap Ideas

- Alarm with buzzer/vibration motor support
- Stopwatch improvements (lap support)
- Sleep tracking + deep sleep optimization
- Phone notifications over BLE (calls/messages)
- Better UI icons and animations

---

## 📄 License

This project is provided as-is for educational and maker use.
You may modify and distribute with credit.

---

## 🙌 Credits

Built using ESP32 + OLED + sensor libraries from the Arduino ecosystem.
