
/*
  ESP32 Smartwatch Project (OLED + RTC + MAX30102 + GPS + MPU6050 + 3 Buttons)
  --------------------------------------------------------------------------
  Faces:
   Face 1: Digital time + date (top), battery (top-right), city/location label (bottom)
   Face 2: Analog watch (real-time)
   Face 3: Health face (HR, SpO2, Steps) + animated progress ring + reset option + send to phone (BLE)
   Face 4: Location + Weather face (GPS + weather string from phone over BLE; optional send)
   Face 5: Menu (Alarm, Stopwatch, Settings, Game placeholder)

  Buttons (3):
   BTN_UP    = next / increase / down in menu
   BTN_OK    = select / start / toggle
   BTN_BACK  = back / decrease / previous in menu / long press sleep (optional)

  Notes about City name:
   - GPS does NOT provide city name by itself. To show a city like “Onujayi”, you need reverse geocoding
     (internet) or phone/app to send city name. This sketch supports receiving city/weather from phone via BLE.

  RTC Fix:
   - Uses RTClib + DS3231 by default.
   - If RTC lost power, it sets RTC time to the compile time ONCE (you can also set time from Settings menu).

  Tested libraries (Arduino IDE, ESP32):
   - U8g2
   - RTClib (Adafruit)
   - TinyGPSPlus
   - MAX30105 (SparkFun) + spo2_algorithm.h
   - Adafruit_MPU6050 + Adafruit_Sensor
   - ESP32 BLE (BLEDevice.h)
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_sleep.h"

// -------------------- Pins --------------------
static const uint8_t BTN_UP   = 32; // next / increase
static const uint8_t BTN_OK   = 33; // select
static const uint8_t BTN_BACK = 25; // back / decrease (also wake pin for deep sleep)

static const uint8_t GPS_RX = 16;   // ESP32 RX2  <- GPS TX
static const uint8_t GPS_TX = 17;   // ESP32 TX2  -> GPS RX

// Battery measurement (change these for your hardware!)
static const uint8_t BAT_ADC_PIN = 34; // must be ADC pin (input only)
static const float   ADC_REF_V   = 3.3f;   // ESP32 ADC reference
static const float   ADC_MAX     = 4095.0f;

// If you use a voltage divider:
// Vbat -> R1 -> ADC -> R2 -> GND, divider ratio = (R1+R2)/R2
// Example: R1=100k, R2=100k => ratio=2.0
static const float BAT_DIVIDER_RATIO = 2.0f;

// LiPo typical range
static const float BAT_V_MIN = 3.30f; // empty
static const float BAT_V_MAX = 4.20f; // full

// Optional buzzer pin for alarm (set to 255 if none)
static const uint8_t BUZZER_PIN = 255;

// -------------------- Display --------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// -------------------- RTC --------------------
RTC_DS3231 rtc;
bool rtcOk = false;

// -------------------- GPS --------------------
TinyGPSPlus gps;

// -------------------- MAX30102 (MAX30105 lib) --------------------
MAX30105 particleSensor;
static const uint16_t HR_SPO2_BUFFER = 100;
uint32_t irBuffer[HR_SPO2_BUFFER];
uint32_t redBuffer[HR_SPO2_BUFFER];

int32_t spo2 = -1;
int8_t  spo2Valid = 0;
int32_t heartRate = -1;
int8_t  hrValid = 0;

// -------------------- MPU6050 --------------------
Adafruit_MPU6050 mpu;
bool mpuOk = false;

// -------------------- Storage --------------------
Preferences prefs;

// -------------------- BLE (Phone Sync) --------------------
// Custom service + characteristics (simple strings; easy to parse on mobile)
static const char* BLE_DEVICE_NAME = "ESP32_SmartWatch";

// 128-bit UUIDs (random)
static BLEUUID SERVICE_UUID       ("8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c01");
static BLEUUID CHAR_HEALTH_UUID   ("8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c02"); // notify: "hr,spo2,steps"
static BLEUUID CHAR_LOCATION_UUID ("8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c03"); // notify: "lat,lon,speed,sats"
static BLEUUID CHAR_WEATHER_UUID  ("8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c04"); // read/write: "City|TempC|Cond"
static BLEUUID CHAR_CMD_UUID      ("8b7f4d5c-6e0e-4c6c-9b51-10f0c74a5c05"); // write: commands

BLEServer* pServer = nullptr;
BLECharacteristic* chHealth = nullptr;
BLECharacteristic* chLoc = nullptr;
BLECharacteristic* chWeather = nullptr;
BLECharacteristic* chCmd = nullptr;

bool bleConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override { bleConnected = false; }
};

// Weather/city (from phone)
String weatherString = "NoWeather";
String cityString    = "NoCity";

// -------------------- UI / Faces --------------------
enum Screen : uint8_t {
  FACE_1_DIGITAL = 0,
  FACE_2_ANALOG  = 1,
  FACE_3_HEALTH  = 2,
  FACE_4_LOCWTH  = 3,
  FACE_5_MENU    = 4,

  MENU_ALARM     = 10,
  MENU_STOPWATCH = 11,
  MENU_SETTINGS  = 12,
  MENU_GAME      = 13
};

Screen screen = FACE_1_DIGITAL;

static const uint8_t MAIN_FACE_COUNT = 5;

enum MenuItem : uint8_t { MI_ALARM, MI_STOPWATCH, MI_SETTINGS, MI_GAME, MI_COUNT };
MenuItem menuSel = MI_ALARM;

// -------------------- Stopwatch --------------------
bool swRunning = false;
uint32_t swStartMs = 0;
uint32_t swElapsedMs = 0;

// -------------------- Alarm --------------------
struct AlarmConfig {
  bool enabled;
  uint8_t hour;
  uint8_t minute;
};
AlarmConfig alarmCfg{false, 7, 0};
bool alarmRinging = false;
uint32_t alarmRingStarted = 0;

// -------------------- Time display --------------------
bool use24h = true;
bool showSecondHand = true;

// -------------------- Steps --------------------
uint32_t stepCount = 0;
// simple step detector
float accelMagAvg = 9.81f;          // running average magnitude (m/s^2)
bool  stepHigh = false;
uint32_t lastStepMs = 0;

// -------------------- Health measurement state machine --------------------
enum HealthState : uint8_t { H_IDLE, H_WAIT_FINGER, H_MEASURING, H_SHOW };
HealthState healthState = H_WAIT_FINGER;
uint16_t healthIdx = 0;
uint32_t lastHealthSampleMs = 0;
uint32_t healthStartedMs = 0;

// -------------------- Buttons (debounce + short/long) --------------------
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MS = 800;

struct ButtonState {
  uint8_t pin;
  bool stable;          // stable level (true = released, false = pressed because pullup)
  bool lastRead;
  uint32_t lastChangeMs;
  uint32_t pressedAtMs;
  bool longFired;
};

ButtonState bUp   {BTN_UP,   true, true, 0, 0, false};
ButtonState bOk   {BTN_OK,   true, true, 0, 0, false};
ButtonState bBack {BTN_BACK, true, true, 0, 0, false};

struct ButtonEvents {
  bool upShort=false, upLong=false;
  bool okShort=false, okLong=false;
  bool backShort=false, backLong=false;
} ev;

static bool readPressed(uint8_t pin) { return digitalRead(pin) == LOW; }

static void updateButton(ButtonState &b, bool &shortEv, bool &longEv) {
  const bool readLevel = digitalRead(b.pin); // HIGH released, LOW pressed
  if (readLevel != b.lastRead) {
    b.lastChangeMs = millis();
    b.lastRead = readLevel;
  }
  if (millis() - b.lastChangeMs > DEBOUNCE_MS) {
    if (readLevel != b.stable) {
      b.stable = readLevel;
      // state changed after debounce
      if (b.stable == LOW) { // pressed
        b.pressedAtMs = millis();
        b.longFired = false;
      } else { // released
        const uint32_t held = millis() - b.pressedAtMs;
        if (!b.longFired && held < LONG_MS) shortEv = true;
      }
    } else if (b.stable == LOW && !b.longFired) {
      // held down
      if (millis() - b.pressedAtMs >= LONG_MS) {
        b.longFired = true;
        longEv = true;
      }
    }
  }
}

static void pollButtons() {
  ev = {};
  updateButton(bUp, ev.upShort, ev.upLong);
  updateButton(bOk, ev.okShort, ev.okLong);
  updateButton(bBack, ev.backShort, ev.backLong);
}

// -------------------- Helpers --------------------
static int clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

static uint8_t batteryPercent() {
  // ESP32 ADC is noisy; average a few reads
  uint32_t sum = 0;
  for (int i=0;i<10;i++) { sum += analogRead(BAT_ADC_PIN); delay(2); }
  float raw = (float)sum / 10.0f;
  float vAdc = (raw / ADC_MAX) * ADC_REF_V;
  float vBat = vAdc * BAT_DIVIDER_RATIO;

  float pct = (vBat - BAT_V_MIN) * 100.0f / (BAT_V_MAX - BAT_V_MIN);
  pct = (pct < 0) ? 0 : ((pct > 100) ? 100 : pct);
  return (uint8_t)(pct + 0.5f);
}

static String dayName(uint8_t dow) {
  // RTClib: 0=Sunday..6=Saturday
  static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  if (dow > 6) dow = 0;
  return String(days[dow]);
}

static void drawBatteryTopRight() {
  uint8_t pct = batteryPercent();
  u8g2.setFont(u8g2_font_5x7_tf);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", pct);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr(128 - w - 1, 8, buf);
}

static void drawProgressRing(int cx, int cy, int r, uint8_t progress /*0-100*/) {
  // Draw 36 segments (10 degrees each)
  uint8_t seg = (uint8_t)((progress * 36) / 100);
  for (uint8_t i=0;i<36;i++) {
    float a0 = (i * 10.0f - 90.0f) * DEG_TO_RAD;
    float a1 = ((i+1) * 10.0f - 90.0f) * DEG_TO_RAD;
    int x0 = cx + (int)(r * cosf(a0));
    int y0 = cy + (int)(r * sinf(a0));
    int x1 = cx + (int)(r * cosf(a1));
    int y1 = cy + (int)(r * sinf(a1));
    if (i <= seg) u8g2.drawLine(x0, y0, x1, y1);
  }
}

// -------------------- BLE callbacks for incoming data --------------------
class WeatherCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String s = c->getValue().c_str();
    if (s.length() == 0) return;
    // expected: "City|TempC|Cond"  e.g. "Chattogram|28|Cloudy"
    weatherString = s;
    int p1 = s.indexOf('|');
    if (p1 > 0) cityString = s.substring(0, p1);
    prefs.putString("weather", weatherString);
    prefs.putString("city", cityString);
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String cmd = c->getValue().c_str();
    if (cmd.length() == 0) return;
    cmd.trim();
    cmd.toUpperCase();

    if (cmd == "RESET_HEALTH") {
      spo2 = -1; heartRate = -1; spo2Valid = 0; hrValid = 0;
    } else if (cmd == "RESET_STEPS") {
      stepCount = 0;
      prefs.putUInt("steps", stepCount);
    } else if (cmd.startsWith("CITY=")) {
      cityString = cmd.substring(5);
      prefs.putString("city", cityString);
    }
  }
};

static void setupBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *service = pServer->createService(SERVICE_UUID);

  chHealth = service->createCharacteristic(
    CHAR_HEALTH_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  chHealth->addDescriptor(new BLE2902());
  chHealth->setValue("hr=-,spo2=-,steps=0");

  chLoc = service->createCharacteristic(
    CHAR_LOCATION_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  chLoc->addDescriptor(new BLE2902());
  chLoc->setValue("lat=0,lon=0,speed=0,sats=0");

  chWeather = service->createCharacteristic(
    CHAR_WEATHER_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  chWeather->setCallbacks(new WeatherCallbacks());
  chWeather->setValue("NoCity|--|--");

  chCmd = service->createCharacteristic(
    CHAR_CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  chCmd->setCallbacks(new CmdCallbacks());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();
}

// -------------------- RTC init + fix --------------------
static void setupRTC() {
  rtcOk = rtc.begin();
  if (!rtcOk) return;

  // If lost power, set to compile time (better than a fixed hardcoded date)
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

// -------------------- Sensors init --------------------
static void setupMAX30102() {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    // sensor not found
    return;
  }
  particleSensor.setup();                // default setup
  particleSensor.setPulseAmplitudeRed(0x2F);
  particleSensor.setPulseAmplitudeIR(0x2F);
}

static void setupMPU() {
  mpuOk = mpu.begin();
  if (!mpuOk) return;
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

// -------------------- Persistent settings --------------------
static void loadSettings() {
  prefs.begin("sw", false);

  use24h = prefs.getBool("use24h", true);
  showSecondHand = prefs.getBool("secHand", true);

  alarmCfg.enabled = prefs.getBool("alarmEn", false);
  alarmCfg.hour    = prefs.getUChar("alarmH", 7);
  alarmCfg.minute  = prefs.getUChar("alarmM", 0);

  stepCount = prefs.getUInt("steps", 0);

  weatherString = prefs.getString("weather", "NoCity|--|--");
  cityString    = prefs.getString("city", "NoCity");
}

static void saveAlarm() {
  prefs.putBool("alarmEn", alarmCfg.enabled);
  prefs.putUChar("alarmH", alarmCfg.hour);
  prefs.putUChar("alarmM", alarmCfg.minute);
}

// -------------------- Step counting --------------------
static void updateSteps() {
  if (!mpuOk) return;

  static uint32_t lastRead = 0;
  if (millis() - lastRead < 40) return; // ~25 Hz
  lastRead = millis();

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  const float ax = a.acceleration.x;
  const float ay = a.acceleration.y;
  const float az = a.acceleration.z;
  float mag = sqrtf(ax*ax + ay*ay + az*az);

  // running average (low pass)
  accelMagAvg = accelMagAvg * 0.9f + mag * 0.1f;

  // high pass component
  float hp = mag - accelMagAvg;

  // threshold + debounce
  const float TH = 1.3f; // adjust based on your mounting (m/s^2)
  const uint32_t STEP_DEBOUNCE = 300;

  if (!stepHigh && hp > TH && (millis() - lastStepMs) > STEP_DEBOUNCE) {
    stepHigh = true;
    lastStepMs = millis();
    stepCount++;
    if ((stepCount % 10) == 0) prefs.putUInt("steps", stepCount); // reduce flash writes
  }
  if (stepHigh && hp < 0.6f) stepHigh = false;
}

// -------------------- Health measurement --------------------
static bool fingerPresent() {
  uint32_t ir = particleSensor.getIR();
  return ir > 50000; // adjust if needed
}

static void startHealthMeasure() {
  healthIdx = 0;
  heartRate = -1; spo2 = -1; hrValid = 0; spo2Valid = 0;
  healthStartedMs = millis();
  lastHealthSampleMs = 0;
  healthState = H_MEASURING;
}

static void updateHealth() {
  // Face 3 runs this continuously
  if (healthState == H_WAIT_FINGER) {
    if (particleSensor.getIR() > 50000) healthState = H_IDLE; // finger is there but idle until user starts
    return;
  }

  if (healthState == H_MEASURING) {
    if (!fingerPresent()) {
      healthState = H_WAIT_FINGER;
      return;
    }

    // sample every ~25ms
    if (millis() - lastHealthSampleMs < 25) return;
    lastHealthSampleMs = millis();

    particleSensor.check();
    if (!particleSensor.available()) return;

    redBuffer[healthIdx] = particleSensor.getRed();
    irBuffer[healthIdx]  = particleSensor.getIR();
    particleSensor.nextSample();

    healthIdx++;
    if (healthIdx >= HR_SPO2_BUFFER) {
      maxim_heart_rate_and_oxygen_saturation(
        irBuffer, HR_SPO2_BUFFER, redBuffer,
        &spo2, &spo2Valid, &heartRate, &hrValid
      );
      healthState = H_SHOW;
      healthStartedMs = millis();
    }
    return;
  }

  if (healthState == H_SHOW) {
    // show for 8 seconds then go idle
    if (millis() - healthStartedMs > 8000) {
      healthState = H_IDLE;
    }
  }
}

// -------------------- GPS update --------------------
static void updateGPS() {
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }
}

// -------------------- Alarm check --------------------
static void updateAlarm(const DateTime &now) {
  if (!alarmCfg.enabled) return;

  if (alarmRinging) {
    if (BUZZER_PIN != 255) {
      // simple beep
      if ((millis() / 200) % 2 == 0) digitalWrite(BUZZER_PIN, HIGH);
      else digitalWrite(BUZZER_PIN, LOW);
    }
    // auto stop after 30s
    if (millis() - alarmRingStarted > 30000) {
      alarmRinging = false;
      if (BUZZER_PIN != 255) digitalWrite(BUZZER_PIN, LOW);
    }
    return;
  }

  // trigger when HH:MM:00
  if (now.hour() == alarmCfg.hour && now.minute() == alarmCfg.minute && now.second() == 0) {
    alarmRinging = true;
    alarmRingStarted = millis();
  }
}

// -------------------- BLE notify updates --------------------
static void bleNotifyLoop() {
  static uint32_t lastNotify = 0;
  if (!bleConnected) return;
  if (millis() - lastNotify < 1000) return;
  lastNotify = millis();

  // health
  char hb[64];
  if (hrValid && heartRate > 40 && heartRate < 200) {
    snprintf(hb, sizeof(hb), "hr=%ld,spo2=%ld,steps=%lu",
             (long)heartRate, (long)(spo2Valid ? spo2 : -1), (unsigned long)stepCount);
  } else {
    snprintf(hb, sizeof(hb), "hr=-,spo2=%ld,steps=%lu",
             (long)(spo2Valid ? spo2 : -1), (unsigned long)stepCount);
  }
  chHealth->setValue((uint8_t*)hb, strlen(hb));
  chHealth->notify();

  // location
  if (gps.location.isValid()) {
    char lb[96];
    snprintf(lb, sizeof(lb), "lat=%.6f,lon=%.6f,speed=%.1f,sats=%lu",
             gps.location.lat(), gps.location.lng(), gps.speed.kmph(),
             (unsigned long)gps.satellites.value());
    chLoc->setValue((uint8_t*)lb, strlen(lb));
    chLoc->notify();
  }
}

// -------------------- Drawing: Faces --------------------
static void drawFace1(const DateTime &t) {
  drawBatteryTopRight();

  // date top
  u8g2.setFont(u8g2_font_6x12_tf);
  char dateBuf[24];
  snprintf(dateBuf, sizeof(dateBuf), "%s %02d/%02d/%04d",
           dayName(t.dayOfTheWeek()).c_str(), t.day(), t.month(), t.year());
  u8g2.drawStr(0, 12, dateBuf);

  // time big
  u8g2.setFont(u8g2_font_logisoso32_tf);
  char timeBuf[10];
  int hh = t.hour();
  if (!use24h) {
    hh = hh % 12; if (hh == 0) hh = 12;
  }
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hh, t.minute());
  int w = u8g2.getStrWidth(timeBuf);
  u8g2.drawStr((128 - w) / 2, 47, timeBuf);

  // seconds small
  u8g2.setFont(u8g2_font_6x12_tf);
  char secBuf[6];
  snprintf(secBuf, sizeof(secBuf), ":%02d", t.second());
  u8g2.drawStr(96, 58, secBuf);

  // bottom: city/location
  u8g2.setFont(u8g2_font_6x12_tf);
  String bottom = cityString;
  if (gps.location.isValid() && (cityString == "NoCity" || cityString.length() == 0)) {
    bottom = "Lat " + String(gps.location.lat(), 2) + " Lon " + String(gps.location.lng(), 2);
  }
  if (!gps.location.isValid() && (cityString == "NoCity")) bottom = "No GPS Fix";
  int bw = u8g2.getStrWidth(bottom.c_str());
  u8g2.drawStr((128 - bw)/2, 64, bottom.c_str());
}

static void drawFace2(const DateTime &t) {
  drawBatteryTopRight();

  // small date
  u8g2.setFont(u8g2_font_6x12_tf);
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", t.day(), t.month());
  u8g2.drawStr(0, 12, dateBuf);

  int cx = 64, cy = 36, r = 28;
  u8g2.drawCircle(cx, cy, r);

  // ticks
  for (int i = 0; i < 12; i++) {
    float a = i * 30 * DEG_TO_RAD;
    int x1 = cx + (r - 2) * sinf(a);
    int y1 = cy - (r - 2) * cosf(a);
    int x2 = cx + r * sinf(a);
    int y2 = cy - r * cosf(a);
    u8g2.drawLine(x1, y1, x2, y2);
  }

  float hA = (t.hour() % 12 + t.minute() / 60.0f) * 30.0f * DEG_TO_RAD;
  float mA = (t.minute() + t.second() / 60.0f) * 6.0f * DEG_TO_RAD;
  float sA = t.second() * 6.0f * DEG_TO_RAD;

  u8g2.drawLine(cx, cy, cx + (int)(14 * sinf(hA)), cy - (int)(14 * cosf(hA)));
  u8g2.drawLine(cx, cy, cx + (int)(22 * sinf(mA)), cy - (int)(22 * cosf(mA)));
  if (showSecondHand) u8g2.drawLine(cx, cy, cx + (int)(25 * sinf(sA)), cy - (int)(25 * cosf(sA)));
  u8g2.drawDisc(cx, cy, 2);

  // day name bottom
  u8g2.setFont(u8g2_font_6x12_tf);
  String dn = dayName(t.dayOfTheWeek());
  int w = u8g2.getStrWidth(dn.c_str());
  u8g2.drawStr((128 - w)/2, 64, dn.c_str());
}

static void drawFace3Health() {
  drawBatteryTopRight();

  // title
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "HEALTH");

  // steps
  u8g2.setCursor(0, 26);
  u8g2.print("Steps: "); u8g2.print(stepCount);

  // HR + SpO2
  u8g2.setCursor(0, 40);
  u8g2.print("HR: ");
  if (hrValid && heartRate > 40 && heartRate < 200) u8g2.print(heartRate);
  else u8g2.print("--");

  u8g2.setCursor(0, 54);
  u8g2.print("SpO2: ");
  if (spo2Valid && spo2 >= 70 && spo2 <= 100) { u8g2.print(spo2); u8g2.print("%"); }
  else u8g2.print("--");

  // progress / status ring at right
  int cx=105, cy=40, r=18;
  u8g2.drawCircle(cx, cy, r);

  if (healthState == H_MEASURING) {
    uint8_t p = (uint8_t)((healthIdx * 100) / HR_SPO2_BUFFER);
    drawProgressRing(cx, cy, r, p);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(74, 64, "OK:meas  BACK:menu");
  } else if (healthState == H_WAIT_FINGER) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(62, 64, "Place finger + OK");
  } else {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(60, 64, "OK:measure  OK(L):reset");
  }

  // simple alerts (fault detection)
  if (spo2Valid && spo2 < 90) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(80, 12, "LOW O2!");
  }
}

static String weatherLine1() {
  // parse: City|Temp|Cond
  int p1 = weatherString.indexOf('|');
  int p2 = weatherString.indexOf('|', p1+1);
  if (p1 < 0 || p2 < 0) return weatherString;
  String city = weatherString.substring(0,p1);
  String temp = weatherString.substring(p1+1,p2);
  String cond = weatherString.substring(p2+1);
  return city + " " + temp + "C";
}

static String weatherLine2() {
  int p1 = weatherString.indexOf('|');
  int p2 = weatherString.indexOf('|', p1+1);
  if (p1 < 0 || p2 < 0) return "";
  return weatherString.substring(p2+1);
}

static void drawFace4LocWeather() {
  drawBatteryTopRight();

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "LOCATION + WEATHER");

  // City / weather from phone
  String wl1 = weatherLine1();
  String wl2 = weatherLine2();
  u8g2.setCursor(0, 26); u8g2.print(wl1);
  u8g2.setCursor(0, 38); u8g2.print(wl2);

  // GPS info
  if (gps.location.isValid()) {
    u8g2.setCursor(0, 52);
    u8g2.print("Lat "); u8g2.print(gps.location.lat(), 4);
    u8g2.setCursor(0, 64);
    u8g2.print("Lon "); u8g2.print(gps.location.lng(), 4);
  } else {
    u8g2.setCursor(0, 56);
    u8g2.print("GPS: No Fix  Sats: ");
    u8g2.print(gps.satellites.value());
  }

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(62, 12, "OK:send phone");
}

static void drawFace5Menu() {
  drawBatteryTopRight();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "MENU");

  const char* items[MI_COUNT] = {"Alarm", "Stopwatch", "Settings", "Game"};
  for (uint8_t i=0;i<MI_COUNT;i++) {
    int y = 26 + i*10;
    if (i == menuSel) u8g2.drawBox(0, y-8, 128, 10);
    u8g2.setDrawColor(i == menuSel ? 0 : 1);
    u8g2.drawStr(2, y, items[i]);
    u8g2.setDrawColor(1);
  }

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 64, "UP/DN:move  OK:enter  BACK:faces");
}

static void drawAlarmScreen(const DateTime &now) {
  // ringing screen
  if ((millis()/250)%2==0) u8g2.drawBox(0,0,128,64), u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_logisoso20_tf);
  u8g2.drawStr(18, 35, "ALARM!");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(10, 55, "Press BACK to stop");
  u8g2.setDrawColor(1);
}

static void drawMenuAlarm(const DateTime &now) {
  drawBatteryTopRight();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "ALARM SET");

  // show current time
  u8g2.setCursor(0, 26);
  u8g2.print("Now: ");
  char buf[10];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  u8g2.print(buf);

  // show alarm config
  u8g2.setCursor(0, 42);
  u8g2.print("Alarm: ");
  snprintf(buf, sizeof(buf), "%02u:%02u", alarmCfg.hour, alarmCfg.minute);
  u8g2.print(buf);
  u8g2.print(alarmCfg.enabled ? " ON" : " OFF");

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 64, "UP:+min  DN:-min  OK:toggle  BACK:menu");
}

static void drawMenuStopwatch() {
  drawBatteryTopRight();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "STOPWATCH");

  if (swRunning) swElapsedMs = millis() - swStartMs;

  uint32_t total = swElapsedMs / 1000;
  uint32_t mins = (total / 60) % 60;
  uint32_t secs = total % 60;
  uint32_t cs   = (swElapsedMs % 1000) / 10;

  u8g2.setFont(u8g2_font_logisoso20_tf);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", (unsigned long)mins, (unsigned long)secs, (unsigned long)cs);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128-w)/2, 42, buf);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 64, "OK:start/stop  UP:reset  BACK:menu");
}

// Settings editor: fields for time/date
enum SetField : uint8_t { SF_HH, SF_MM, SF_DD, SF_MO, SF_YY, SF_DONE };
SetField setField = SF_HH;
DateTime editDT(2026,1,1,0,0,0);
bool editingLoaded = false;

static void drawMenuSettings(const DateTime &now) {
  drawBatteryTopRight();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "SETTINGS");

  if (!editingLoaded) {
    editDT = now;
    editingLoaded = true;
    setField = SF_HH;
  }

  // mode toggles
  u8g2.setCursor(0, 26);
  u8g2.print("24H: "); u8g2.print(use24h ? "ON" : "OFF");
  u8g2.setCursor(0, 38);
  u8g2.print("2nd: "); u8g2.print(showSecondHand ? "ON" : "OFF");

  // time/date editor
  u8g2.setCursor(0, 52);
  u8g2.print("Edit: ");

  char buf[24];
  snprintf(buf, sizeof(buf), "%02d:%02d  %02d/%02d/%04d",
           editDT.hour(), editDT.minute(), editDT.day(), editDT.month(), editDT.year());
  u8g2.print(buf);

  // cursor indicator
  int x = 0;
  if (setField == SF_HH) x = 34;
  else if (setField == SF_MM) x = 34 + 3*6;
  else if (setField == SF_DD) x = 34 + 7*6;
  else if (setField == SF_MO) x = 34 + 10*6;
  else if (setField == SF_YY) x = 34 + 13*6;
  u8g2.drawLine(x, 54, x+12, 54);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 64, "UP/DN:change  OK:next  OK(L):save  BACK:menu");
}

static void drawMenuGame() {
  drawBatteryTopRight();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "GAME (Placeholder)");
  u8g2.drawStr(0, 28, "You can add later.");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 64, "BACK:menu");
}

// -------------------- Navigation / Input handling --------------------
static void gotoMainFace(uint8_t idx) {
  idx %= MAIN_FACE_COUNT;
  screen = (Screen)idx;
}

static void nextMainFace() {
  uint8_t idx = (uint8_t)screen;
  if (idx >= MAIN_FACE_COUNT) idx = 0;
  idx = (idx + 1) % MAIN_FACE_COUNT;
  gotoMainFace(idx);
}

static void goMenu() { screen = FACE_5_MENU; }

static void stopAlarmRing() {
  alarmRinging = false;
  if (BUZZER_PIN != 255) digitalWrite(BUZZER_PIN, LOW);
}

static void handleMainFaceButtons() {
  // From face 1..4:
  if (ev.upShort) nextMainFace();
  if (ev.backShort) goMenu();

  if (screen == FACE_1_DIGITAL) {
    if (ev.okShort) { use24h = !use24h; prefs.putBool("use24h", use24h); }
  } else if (screen == FACE_2_ANALOG) {
    if (ev.okShort) { showSecondHand = !showSecondHand; prefs.putBool("secHand", showSecondHand); }
  } else if (screen == FACE_3_HEALTH) {
    if (ev.okShort) {
      if (healthState == H_WAIT_FINGER) {
        // can't start; needs finger
      } else {
        startHealthMeasure();
      }
    }
    if (ev.okLong) {
      // reset values + steps
      spo2=-1; heartRate=-1; spo2Valid=0; hrValid=0;
      stepCount = 0;
      prefs.putUInt("steps", stepCount);
    }
  } else if (screen == FACE_4_LOCWTH) {
    if (ev.okShort && bleConnected) {
      // send immediately (one-shot notify)
      bleNotifyLoop();
    }
  }

  // Optional deep sleep: long BACK from any main face
  if (ev.backLong) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(20, 32, "Sleeping...");
    u8g2.sendBuffer();
    delay(400);

    u8g2.setPowerSave(1);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_BACK, 0); // wake on BACK press
    esp_deep_sleep_start();
  }
}

static void handleMenuButtons() {
  if (ev.backShort) { // exit to faces
    gotoMainFace(0);
    return;
  }

  if (ev.upShort) {
    menuSel = (MenuItem)((menuSel + 1) % MI_COUNT);
  }
  if (ev.backLong) {
    menuSel = (MenuItem)((menuSel + MI_COUNT - 1) % MI_COUNT);
  }
  // Use BACK long for "previous item" (because we only have 3 buttons)
  // If you don't like this, swap behavior as you want.

  if (ev.okShort) {
    switch (menuSel) {
      case MI_ALARM:     screen = MENU_ALARM;     break;
      case MI_STOPWATCH: screen = MENU_STOPWATCH; break;
      case MI_SETTINGS:  screen = MENU_SETTINGS;  editingLoaded = false; break;
      case MI_GAME:      screen = MENU_GAME;      break;
      default: break;
    }
  }
}

static void handleAlarmButtons() {
  if (ev.backShort) { screen = FACE_5_MENU; return; }

  if (ev.okShort) {
    alarmCfg.enabled = !alarmCfg.enabled;
    saveAlarm();
  }
  if (ev.upShort) {
    // +1 minute
    uint16_t m = (uint16_t)alarmCfg.hour * 60 + alarmCfg.minute;
    m = (m + 1) % (24*60);
    alarmCfg.hour = m / 60;
    alarmCfg.minute = m % 60;
    saveAlarm();
  }
  if (ev.backLong) {
    // -1 minute
    int m = (int)alarmCfg.hour * 60 + alarmCfg.minute;
    m = (m - 1);
    if (m < 0) m = 24*60 - 1;
    alarmCfg.hour = m / 60;
    alarmCfg.minute = m % 60;
    saveAlarm();
  }
}

static void handleStopwatchButtons() {
  if (ev.backShort) { screen = FACE_5_MENU; return; }

  if (ev.okShort) {
    if (!swRunning) {
      swRunning = true;
      swStartMs = millis() - swElapsedMs;
    } else {
      swRunning = false;
    }
  }
  if (ev.upShort && !swRunning) {
    swElapsedMs = 0;
  }
}

static void handleSettingsButtons() {
  if (ev.backShort) { screen = FACE_5_MENU; return; }

  // quick toggles
  if (ev.okShort && setField == SF_DONE) {
    setField = SF_HH;
  }

  // change field with UP/DN
  auto inc = [&](int delta) {
    int hh = editDT.hour();
    int mm = editDT.minute();
    int dd = editDT.day();
    int mo = editDT.month();
    int yy = editDT.year();

    if (setField == SF_HH) hh = (hh + delta + 24) % 24;
    else if (setField == SF_MM) mm = (mm + delta + 60) % 60;
    else if (setField == SF_DD) dd = clampi(dd + delta, 1, 31);
    else if (setField == SF_MO) mo = clampi(mo + delta, 1, 12);
    else if (setField == SF_YY) yy = clampi(yy + delta, 2020, 2099);

    // Keep day valid (rough)
    dd = clampi(dd, 1, 31);
    editDT = DateTime(yy, mo, dd, hh, mm, 0);
  };

  if (ev.upShort) inc(+1);
  if (ev.backLong) inc(-1);

  // next field
  if (ev.okShort) {
    if (setField == SF_HH) setField = SF_MM;
    else if (setField == SF_MM) setField = SF_DD;
    else if (setField == SF_DD) setField = SF_MO;
    else if (setField == SF_MO) setField = SF_YY;
    else if (setField == SF_YY) setField = SF_DONE;
    else setField = SF_HH;
  }

  // save
  if (ev.okLong) {
    use24h = !use24h; // optional: long OK toggles 24h quickly
    prefs.putBool("use24h", use24h);

    // Actually set RTC with edited date/time
    if (rtcOk) rtc.adjust(editDT);
  }
}

static void handleGameButtons() {
  if (ev.backShort) { screen = FACE_5_MENU; return; }
}

// -------------------- Setup + loop --------------------
void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  if (BUZZER_PIN != 255) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db); // helps read higher voltage via divider

  Wire.begin(21, 22);
  u8g2.begin();
  u8g2.setPowerSave(0);

  loadSettings();
  setupRTC();
  setupMAX30102();
  setupMPU();

  // GPS
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  setupBLE();

  // UI splash
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(10, 28, "Smart Watch");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(10, 48, "UP:Next  BACK:Menu");
  u8g2.sendBuffer();
  delay(800);
}

void loop() {
  pollButtons();

  // read sensors
  updateGPS();
  updateSteps();
  updateHealth();

  // time
  DateTime now = rtcOk ? rtc.now() : DateTime(2026,1,1,0,0,0);

  // alarm
  updateAlarm(now);

  // stop ringing
  if (alarmRinging && (ev.backShort || ev.okShort)) stopAlarmRing();

  // navigation
  if (alarmRinging) {
    // ignore other navigation
  } else {
    if (screen <= FACE_4_LOCWTH) handleMainFaceButtons();
    else if (screen == FACE_5_MENU) handleMenuButtons();
    else if (screen == MENU_ALARM) handleAlarmButtons();
    else if (screen == MENU_STOPWATCH) handleStopwatchButtons();
    else if (screen == MENU_SETTINGS) handleSettingsButtons();
    else if (screen == MENU_GAME) handleGameButtons();
  }

  // BLE notify
  bleNotifyLoop();

  // draw
  u8g2.clearBuffer();

  if (!rtcOk) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "RTC not found!");
    u8g2.drawStr(0, 26, "Check wiring SDA=21");
    u8g2.drawStr(0, 38, "SCL=22, VCC, GND");
    u8g2.drawStr(0, 52, "Then restart.");
  } else if (alarmRinging) {
    drawAlarmScreen(now);
  } else {
    switch (screen) {
      case FACE_1_DIGITAL: drawFace1(now); break;
      case FACE_2_ANALOG:  drawFace2(now); break;
      case FACE_3_HEALTH:  drawFace3Health(); break;
      case FACE_4_LOCWTH:  drawFace4LocWeather(); break;
      case FACE_5_MENU:    drawFace5Menu(); break;
      case MENU_ALARM:     drawMenuAlarm(now); break;
      case MENU_STOPWATCH: drawMenuStopwatch(); break;
      case MENU_SETTINGS:  drawMenuSettings(now); break;
      case MENU_GAME:      drawMenuGame(); break;
      default:             drawFace1(now); break;
    }
  }

  u8g2.sendBuffer();
  delay(30);
}
