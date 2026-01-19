#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <RTClib.h>
#include <math.h>
#include "esp_sleep.h"
#include "MAX30105.h"
#include "spo2_algorithm.h"

// --- Pins ---
#define BTN_FACE  32  // মোড পরিবর্তন
#define BTN_SW    33  // স্টপওয়াচ/অন্যান্য কন্ট্রোল
#define BTN_SLEEP 25  // স্লিপ/ব্যাক

// --- WiFi & Telegram ---
const char* ssid = "CUET-Students";
const char* password = "1020304050";
#define BOTtoken "7993041830:AAHaDNXBUDgCBCSzmuhDEWCWCX7RAOx5jU8"
#define CHAT_ID "5601276186"

// --- OLED & Sensors ---
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
RTC_DS3231 rtc;
TinyGPSPlus gps;
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
MAX30105 particleSensor;

// --- ঘড়ির ফেস ডেফিনেশন ---
enum WatchFace { 
  FACE_DIGITAL, 
  FACE_ANALOG, 
  FACE_DATE, 
  FACE_STOPWATCH 
};

// --- মোড ডেফিনেশন ---
enum AppMode { 
  MODE_SPLASH,    // স্প্লাশ স্ক্রিন
  MODE_CLOCK,     // ঘড়ি (ডিজিটাল/এনালগ/তারিখ/স্টপওয়াচ)
  MODE_GPS,       // GPS ট্র্যাকার
  MODE_HEALTH     // হার্ট রেট ও অক্সিজেন
};

// --- গ্লোবাল ভেরিয়েবল ---
AppMode currentMode = MODE_SPLASH;
WatchFace currentFace = FACE_ANALOG; // ঘড়ির সাব-মোড
bool alarmActive = false;
bool swRunning = false;
unsigned long swStart = 0;
unsigned long swElapsed = 0;
unsigned long btnPressTime = 0;
unsigned long splashStart = 0;

// --- GPS ভেরিয়েবল ---
unsigned long lastBotCheck = 0;
const unsigned long botInterval = 2000;

// --- Health ভেরিয়েবল ---
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Health মোডের স্টেট মেশিন
enum HealthState {
  HEALTH_NO_FINGER,
  HEALTH_MEASURING,
  HEALTH_SHOWING_RESULT
};
HealthState healthState = HEALTH_NO_FINGER;
unsigned long healthMeasureStart = 0;

// --- ফাংশন প্রোটোটাইপ ---
void handleButtons();
void drawSplashScreen();
void drawClockMode();
void drawGPSMode();
void drawHealthMode();
void enterSleep();
void handleClockButtons();
void handleTelegram();

// ঘড়ির ফেস ফাংশন
void drawDigital(DateTime t);
void drawAnalog(DateTime t);
void drawDate(DateTime t);
void drawStopwatch();
void drawAlarmScreen();

// === সেটআপ ===
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17); // GPS: RX=16, TX=17

  Wire.begin(21, 22);
  u8g2.begin();
  rtc.begin();

  // RTC সেটআপ
  if (rtc.lostPower()) {
    // সঠিক তারিখ সেট করুন (YYYY, MM, DD, HH, MM, SS)
    rtc.adjust(DateTime(2026, 1, 20, 12, 0, 0));
  }

  // MAX30105 সেন্সর
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 not found!");
  } else {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x2F);
    particleSensor.setPulseAmplitudeIR(0x2F);
  }

  // বাটন সেটআপ
  pinMode(BTN_FACE, INPUT_PULLUP);
  pinMode(BTN_SW, INPUT_PULLUP);
  pinMode(BTN_SLEEP, INPUT_PULLUP);

  // স্লিপ মোড
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_25, 0);

  // স্প্লাশ স্ক্রিন টাইমার
  splashStart = millis();

  // WiFi কানেক্ট (GPS মোডের জন্য)
  WiFi.begin(ssid, password);
  client.setInsecure();
  
  // WiFi কানেকশন নন-ব্লকিংভাবে
  Serial.println("Connecting to WiFi...");
}

// === স্প্লাশ স্ক্রিন ===
void drawSplashScreen() {
  u8g2.setFont(u8g2_font_helvB18_tr);
  u8g2.drawStr(20, 30, "Smart");
  u8g2.drawStr(25, 55, "Watch");
  
  // "Loading..." টেক্সট
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(40, 65, "Loading...");
}

// === মূল লুপ ===
void loop() {
  // স্প্লাশ স্ক্রিন টাইমার চেক
  if (currentMode == MODE_SPLASH) {
    if (millis() - splashStart > 2000) { // 2 সেকেন্ড পর
      currentMode = MODE_CLOCK;
    }
  }

  handleButtons();

  // GPS ডেটা পড়া (সব সময়)
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }

  u8g2.clearBuffer();

  // বর্তমান মোড অনুযায়ী ডিসপ্লে
  switch (currentMode) {
    case MODE_SPLASH:
      drawSplashScreen();
      break;
    case MODE_CLOCK:
      drawClockMode();
      break;
    case MODE_GPS:
      drawGPSMode();
      if (millis() - lastBotCheck > botInterval) {
        handleTelegram();
        lastBotCheck = millis();
      }
      break;
    case MODE_HEALTH:
      drawHealthMode();
      break;
  }

  u8g2.sendBuffer();
  delay(50);
}

// === বাটন হ্যান্ডলার ===
void handleButtons() {
  static uint32_t lastModePress = 0;
  static uint32_t lastFacePress = 0;

  // স্প্লাশ স্ক্রিনে থাকলে মোড পরিবর্তন বন্ধ
  if (currentMode == MODE_SPLASH) {
    return;
  }

  // মোড পরিবর্তন (BTN_FACE সংক্ষিপ্ত প্রেস)
  if (digitalRead(BTN_FACE) == LOW && (millis() - lastModePress > 300)) {
    currentMode = (AppMode)((currentMode + 1) % 4);
    if (currentMode == MODE_SPLASH) currentMode = MODE_CLOCK; // স্প্লাশ স্কিপ
    lastModePress = millis();
    lastFacePress = millis(); // ফেস পরিবর্তন ডিবাউন্স
    return;
  }

  // প্রতিটি মোডের ভিতরের বাটন লজিক
  switch (currentMode) {
    case MODE_CLOCK:
      handleClockButtons();
      break;
    case MODE_GPS:
      // GPS মোডে অতিরিক্ত বাটন লজিক (যদি প্রয়োজন হয়)
      break;
    case MODE_HEALTH:
      // Health মোডে বাটন লজিক - নতুন মাপ শুরু
      if (digitalRead(BTN_SW) == LOW) {
        healthState = HEALTH_NO_FINGER;
        delay(200); // ডিবাউন্স
      }
      break;
  }

  // স্লিপ/ব্যাক বাটন
  if (digitalRead(BTN_SLEEP) == LOW) {
    delay(200);
    if (alarmActive) alarmActive = false;
    else enterSleep();
  }
}

// === ঘড়ি মোডের বাটন লজিক ===
void handleClockButtons() {
  static uint32_t lastFacePress = 0;

  // ফেস পরিবর্তন (BTN_FACE প্রেস)
  if (digitalRead(BTN_FACE) == LOW && (millis() - lastFacePress > 500)) {
    currentFace = (WatchFace)((currentFace + 1) % 4);
    lastFacePress = millis();
    delay(200); // ডিবাউন্স
  }

  // স্টপওয়াচ কন্ট্রোল
  if (currentFace == FACE_STOPWATCH) {
    if (digitalRead(BTN_SW) == LOW) {
      if (btnPressTime == 0) btnPressTime = millis();
      if (millis() - btnPressTime > 1500) {
        swRunning = false;
        swElapsed = 0;
      }
    } else {
      if (btnPressTime > 0 && millis() - btnPressTime < 1500) {
        if (!swRunning) {
          swRunning = true;
          swStart = millis() - swElapsed;
        } else {
          swRunning = false;
        }
      }
      btnPressTime = 0;
    }
  }
}

// === ঘড়ি মোড ডিসপ্লে ===
void drawClockMode() {
  DateTime now = rtc.now();

  if (alarmActive) {
    drawAlarmScreen();
  } else {
    switch (currentFace) {
      case FACE_DIGITAL:
        drawDigital(now);
        break;
      case FACE_ANALOG:
        drawAnalog(now);
        break;
      case FACE_DATE:
        drawDate(now);
        break;
      case FACE_STOPWATCH:
        drawStopwatch();
        break;
    }
  }
}

// === GPS মোড ডিসপ্লে ===
void drawGPSMode() {
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "GPS TRACKER");

  if (gps.location.isValid()) {
    u8g2.setCursor(0, 28);
    u8g2.print("Lat: "); u8g2.print(gps.location.lat(), 6);
    u8g2.setCursor(0, 42);
    u8g2.print("Lng: "); u8g2.print(gps.location.lng(), 6);
    u8g2.setCursor(0, 56);
    u8g2.print("Sats: "); u8g2.print(gps.satellites.value());
  } else {
    u8g2.setCursor(0, 35);
    u8g2.print("Searching Sats...");
    u8g2.setCursor(0, 50);
    u8g2.print("Sats: "); u8g2.print(gps.satellites.value());
  }
}

// === হেলথ মোড ডিসপ্লে ===
void drawHealthMode() {
  uint32_t irCheck = particleSensor.getIR();

  switch (healthState) {
    case HEALTH_NO_FINGER:
      if (irCheck < 50000) {
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(15, 35, "Place your finger...");
      } else {
        healthState = HEALTH_MEASURING;
        healthMeasureStart = millis();
      }
      break;
      
    case HEALTH_MEASURING:
      if (irCheck < 50000) {
        healthState = HEALTH_NO_FINGER;
        break;
      }
      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.drawStr(15, 35, "Measuring..Stay Still");
      
      // ৩ সেকেন্ড পর রিডিং নেওয়া শুরু
      if (millis() - healthMeasureStart > 3000) {
        // ১০০টি স্যাম্পল নিন
        for (byte i = 0; i < 100; i++) {
          while (!particleSensor.available())
            particleSensor.check();

          redBuffer[i] = particleSensor.getRed();
          irBuffer[i] = particleSensor.getIR();
          particleSensor.nextSample();
        }

        // ক্যালকুলেশন
        maxim_heart_rate_and_oxygen_saturation(
          irBuffer, 100, redBuffer,
          &spo2, &validSPO2, &heartRate, &validHeartRate
        );
        
        healthState = HEALTH_SHOWING_RESULT;
        healthMeasureStart = millis();
      }
      break;
      
    case HEALTH_SHOWING_RESULT:
      // রেজাল্ট ৫ সেকেন্ড দেখানো
      if (millis() - healthMeasureStart < 5000) {
        // হার্ট রেট প্রদর্শন
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(0, 15, "Heart Rate (BPM):");
        u8g2.setCursor(0, 32);
        if (validHeartRate && heartRate > 40 && heartRate < 180) {
          u8g2.setFont(u8g2_font_ncenB14_tr);
          u8g2.print(heartRate);
        } else {
          u8g2.setFont(u8g2_font_ncenB14_tr);
          u8g2.print("--");
        }

        // SpO2 প্রদর্শন
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(0, 50, "Oxygen (SpO2):");
        u8g2.setCursor(0, 64);
        if (validSPO2 && spo2 > 70 && spo2 <= 100) {
          u8g2.setFont(u8g2_font_ncenB14_tr);
          u8g2.print(spo2); u8g2.print("%");
        } else {
          u8g2.setFont(u8g2_font_ncenB14_tr);
          u8g2.print("--");
        }
      } else {
        // ৫ সেকেন্ড পর আবার শুরু
        healthState = HEALTH_NO_FINGER;
      }
      break;
  }
}

// === টেলিগ্রাম হ্যান্ডলার ===
void handleTelegram() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  while(numNewMessages) {
    for (int i=0; i<numNewMessages; i++) {
      String text = bot.messages[i].text;
      String from_id = String(bot.messages[i].chat_id);

      if (text == "loc") {
        if (gps.location.isValid()) {
          String mapsLink = "https://www.google.com/maps?q=";
          mapsLink += String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
          bot.sendMessage(from_id, "Location: " + mapsLink, "");
        } else {
          bot.sendMessage(from_id, "GPS signal not fixed yet. Sats: " + String(gps.satellites.value()), "");
        }
      }
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}

// === ঘড়ির ফাংশনগুলো ===
void drawDigital(DateTime t) {
  u8g2.setFont(u8g2_font_logisoso32_tf);
  char buf[9];
  sprintf(buf, "%02d:%02d", t.hour(), t.minute());
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 45, buf);
  
  u8g2.setFont(u8g2_font_6x12_tf);
  sprintf(buf, ":%02d", t.second());
  u8g2.drawStr(100, 55, buf);
}

void drawAnalog(DateTime t) {
  int cx = 64, cy = 32, r = 30;
  u8g2.drawCircle(cx, cy, r);

  for (int i = 0; i < 12; i++) {
    float angle = i * 30 * PI / 180;
    int x1 = cx + (r - 2) * sin(angle);
    int y1 = cy - (r - 2) * cos(angle);
    int x2 = cx + r * sin(angle);
    int y2 = cy - r * cos(angle);
    u8g2.drawLine(x1, y1, x2, y2);
  }

  float hA = (t.hour() % 12 + t.minute() / 60.0) * 30 * PI / 180;
  float mA = t.minute() * 6 * PI / 180;
  float sA = t.second() * 6 * PI / 180;

  u8g2.drawLine(cx, cy, cx + 15 * sin(hA), cy - 15 * cos(hA));
  u8g2.drawLine(cx, cy, cx + 22 * sin(mA), cy - 22 * cos(mA));
  u8g2.setDrawColor(1);
  u8g2.drawLine(cx, cy, cx + 26 * sin(sA), cy - 26 * cos(sA));
  u8g2.drawDisc(cx, cy, 2);
}

void drawDate(DateTime t) {
  // সঠিক বার নামের অ্যারে (RTC library 0=Sunday, 1=Monday, ... 6=Saturday)
  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  
  // বারের নাম
  u8g2.setFont(u8g2_font_helvB12_tr);
  String dayStr = String(days[t.dayOfTheWeek()]); // t.dayOfTheWeek() 0-6 রিটার্ন করে
  int w1 = u8g2.getStrWidth(dayStr.c_str());
  u8g2.drawStr((128 - w1) / 2, 25, dayStr.c_str());
  
  // পূর্ণ তারিখ (DD/MM/YYYY) - আপনি চাইলে MM/DD/YYYY করতে পারেন
  u8g2.setFont(u8g2_font_logisoso18_tf);
  char buf[12];
  // DD/MM/YY ফরম্যাট
  sprintf(buf, "%02d/%02d/%02d", t.day(), t.month(), t.year() % 100);
  // অথবা MM/DD/YY ফরম্যাট চাইলে:
  // sprintf(buf, "%02d/%02d/%02d", t.month(), t.day(), t.year() % 100);
  
  int w2 = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w2) / 2, 55, buf);
}

void drawStopwatch() {
  if (swRunning) swElapsed = millis() - swStart;
  
  u8g2.setFont(u8g2_font_logisoso20_tf);
  uint32_t totalSeconds = swElapsed / 1000;
  uint32_t mins = (totalSeconds / 60) % 60;
  uint32_t secs = totalSeconds % 60;
  uint32_t ms = (swElapsed % 1000) / 10;

  char buf[12];
  sprintf(buf, "%02d:%02d", mins, secs);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr(20, 40, buf);

  u8g2.setFont(u8g2_font_6x12_tf);
  sprintf(buf, ".%02d", ms);
  u8g2.drawStr(20 + w, 40, buf);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(15, 60, swRunning ? "RUNNING (HOLD TO RESET)" : "PAUSED (HOLD TO RESET)");
}

void drawAlarmScreen() {
  if (millis() % 600 < 300) { 
    u8g2.drawBox(0, 0, 128, 64); 
    u8g2.setDrawColor(0);
  }
  u8g2.setFont(u8g2_font_logisoso18_tf);
  u8g2.drawStr(25, 35, "ALARM!");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(15, 55, "Press SLEEP to stop");
  u8g2.setDrawColor(1);
}

void enterSleep() {
  // স্লিপিং মোড মেসেজ দেখানো
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(30, 30, "Sleeping...");
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(25, 50, "Press any to wake");
  u8g2.sendBuffer();
  delay(1500); // 1.5 সেকেন্ড মেসেজ দেখানো
  
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setPowerSave(1);
  delay(100);
  esp_deep_sleep_start();
}