#include "secrets.h"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <PZEM004Tv30.h>

#define PZEM_RX 27
#define PZEM_TX 22
#define BACKLIGHT_PIN 21

TFT_eSPI tft = TFT_eSPI();
PZEM004Tv30 pzem(Serial2, PZEM_RX, PZEM_TX, 0xF8);
BlynkTimer timer;
Preferences prefs;

const float COMP_ON_W  = 100.0;
const float COMP_OFF_W = 80.0;
const float WASTE_W    = 50.0;

const unsigned long READ_INTERVAL_MS   = 2000;
const unsigned long WASTE_CONFIRM_MS   = 30000;
const unsigned long WASTE_ALERT_GAP_MS = 300000;
const unsigned long RECONNECT_MS       = 15000;
const unsigned long SAVE_INTERVAL_MS   = 600000;

#define C_BG    TFT_BLACK
#define C_CYAN  0x07FF
#define C_RED   TFT_RED
#define C_GREEN TFT_GREEN
#define C_WHITE TFT_WHITE
#define C_GRAY  TFT_DARKGREY

float vals[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
bool  pzemOk = false;

bool compOn      = false;
bool wasteActive = false;
unsigned long wasteSince = 0;
unsigned long cycleStart = 0;
int   cyclesThisHour = 0;
int   cycleCount     = 0;
float totalCycleMin  = 0;
float wastedWh       = 0;

unsigned long lastRead = 0;
unsigned long lastSave = 0;
unsigned long lastWasteAlert = 0;

const uint16_t cardColors[6] = {0x000F, 0x0400, 0x2008, 0x000F, 0x0400, 0x2008};
const char* labels[6] = {"VOLTAGE", "CURRENT", "POWER", "ENERGY", "FREQUENCY", "PWR FACTOR"};
const char* units[6]  = {"V", "A", "W", "kWh", "Hz", ""};

void drawStaticLayout() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 35, C_CYAN);

  for (int i = 0; i < 6; i++) {
    int x = 4 + (i % 3) * 106;
    int y = 39 + (i / 3) * 88;
    tft.fillRoundRect(x, y, 104, 85, 6, cardColors[i]);
    tft.drawRoundRect(x, y, 104, 85, 6, C_CYAN);
    tft.setTextColor(0xC618, cardColors[i]);
    tft.setTextSize(1);
    tft.setCursor(x + 6, y + 6);
    tft.print(labels[i]);
    tft.drawFastHLine(x + 4, y + 17, 96, C_CYAN);
  }

  tft.drawRect(0, 215, 320, 25, C_CYAN);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(4, 222);
  tft.print("WASTED:");
}

void updateCard(int idx, uint16_t valColor) {
  int x = 4 + (idx % 3) * 106;
  int y = 39 + (idx / 3) * 88;
  float val = vals[idx];

  tft.fillRect(x + 3, y + 20, 98, 61, cardColors[idx]);

  char buf[14];
  if (isnan(val)) {
    strcpy(buf, "---");
    tft.setTextColor(C_GRAY, cardColors[idx]);
  } else {
    if (idx == 1 || idx == 5)  dtostrf(val, 1, 2, buf);
    else if (idx == 3)         dtostrf(val, 1, 3, buf);
    else                       dtostrf(val, 1, 1, buf);
    tft.setTextColor(valColor, cardColors[idx]);
  }

  tft.setTextSize(2);
  int tw = strlen(buf) * 12;
  tft.setCursor(x + (104 - tw) / 2, y + 28);
  tft.print(buf);

  if (!isnan(val) && strlen(units[idx]) > 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_CYAN, cardColors[idx]);
    int uw = strlen(units[idx]) * 6;
    tft.setCursor(x + (104 - uw) / 2, y + 52);
    tft.print(units[idx]);
  }
}

void drawHeader(const char* text, uint16_t bg) {
  tft.fillRect(0, 0, 320, 35, bg);
  tft.setTextColor(TFT_BLACK, bg);
  tft.setTextSize(2);
  int tw = strlen(text) * 12;
  tft.setCursor((320 - tw) / 2, 10);
  tft.print(text);
}

void drawWasted() {
  tft.fillRect(55, 216, 245, 22, C_BG);
  tft.setTextColor(wastedWh > 0 ? C_RED : C_WHITE, C_BG);
  tft.setTextSize(1);
  tft.setCursor(58, 222);
  char buf[30];
  snprintf(buf, sizeof(buf), "%.4f Wh", wastedWh);
  tft.print(buf);
  tft.fillCircle(308, 228, 5, Blynk.connected() ? C_GREEN : C_RED);
}

void readPzem() {
  vals[0] = pzem.voltage();
  vals[1] = pzem.current();
  vals[2] = pzem.power();
  vals[3] = pzem.energy();
  vals[4] = pzem.frequency();
  vals[5] = pzem.pf();
  pzemOk = !isnan(vals[0]) && !isnan(vals[1]) && !isnan(vals[2]);
}

void updateLogic(unsigned long now, float dtHours) {
  float p = vals[2];

  if (!compOn && p >= COMP_ON_W) {
    compOn = true;
    cycleStart = now;
    cyclesThisHour++;
  } else if (compOn && p < COMP_OFF_W) {
    compOn = false;
    float minutes = (now - cycleStart) / 60000.0;
    totalCycleMin += minutes;
    cycleCount++;
    if (minutes > 10.0 && Blynk.connected()) {
      Blynk.logEvent("compressor_anomaly", String("Long compressor cycle: ") + String(minutes, 1) + " min");
    }
  }

  bool wasteCond = (!compOn && p > WASTE_W);
  if (wasteCond) {
    if (wasteSince == 0) wasteSince = now;
    if (now - wasteSince >= WASTE_CONFIRM_MS) wasteActive = true;
  } else {
    wasteSince = 0;
    wasteActive = false;
  }

  if (wasteActive) {
    wastedWh += p * dtHours;
    if (Blynk.connected() && (lastWasteAlert == 0 || now - lastWasteAlert > WASTE_ALERT_GAP_MS)) {
      Blynk.logEvent("energy_waste", String("Energy waste: ") + String(p, 1) + " W while idle");
      lastWasteAlert = now;
    }
  }
}

void sendToBlynk() {
  if (!Blynk.connected() || !pzemOk) return;

  const char* state = compOn ? "COMPRESSOR ON" : (wasteActive ? "WASTE" : "STANDBY OK");

  Blynk.virtualWrite(V0, vals[0]);
  Blynk.virtualWrite(V1, vals[1]);
  Blynk.virtualWrite(V2, vals[2]);
  Blynk.virtualWrite(V3, vals[3]);
  Blynk.virtualWrite(V4, vals[4]);
  Blynk.virtualWrite(V5, vals[5]);
  Blynk.virtualWrite(V6, wastedWh);
  Blynk.virtualWrite(V7, state);
  Blynk.virtualWrite(V8, cyclesThisHour);
  Blynk.virtualWrite(V9, cycleCount > 0 ? totalCycleMin / cycleCount : 0.0f);
}

void sendHourlyReport() {
  float avgCycle = cycleCount > 0 ? totalCycleMin / cycleCount : 0.0;

  String msg = "Wasted: " + String(wastedWh, 3) + " Wh | Cycles: " + String(cyclesThisHour) +
               " | Avg cycle: " + String(avgCycle, 1) + " min";

  if (Blynk.connected()) {
    if (cyclesThisHour > 15) {
      Blynk.logEvent("compressor_anomaly", "Too many cycles. " + msg);
    } else if (avgCycle > 10.0) {
      Blynk.logEvent("compressor_anomaly", "Cycles too long. " + msg);
    } else {
      Blynk.logEvent("hourly_report", msg);
    }
  }

  cyclesThisHour = 0;
  cycleCount = 0;
  totalCycleMin = 0;
}

void maintainConnection() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < RECONNECT_MS) return;
  last = now;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }
  if (!Blynk.connected()) {
    Blynk.connect(3000);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(2);
  tft.setCursor(50, 100);
  tft.print("Smart Power Monitor");
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(90, 130);
  tft.print("Connecting WiFi...");

  prefs.begin("fridge", false);
  wastedWh = prefs.getFloat("waste", 0.0f);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(250);
  }

  Blynk.config(BLYNK_AUTH_TOKEN);
  if (WiFi.status() == WL_CONNECTED) Blynk.connect(3000);

  drawStaticLayout();

  timer.setInterval(2000L, sendToBlynk);
  timer.setInterval(3600000L, sendHourlyReport);

  lastRead = millis();
  lastSave = millis();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) Blynk.run();
  timer.run();
  maintainConnection();

  unsigned long now = millis();

  if (now - lastRead >= READ_INTERVAL_MS) {
    float dtHours = (now - lastRead) / 3600000.0;
    lastRead = now;

    readPzem();

    if (!pzemOk) {
      drawHeader("PZEM ERROR", C_RED);
      Serial.println("PZEM not responding");
    } else {
      updateLogic(now, dtHours);

      if (wasteActive)   drawHeader("!! WASTE !!", C_RED);
      else if (compOn)   drawHeader("COMPRESSOR ON", C_CYAN);
      else               drawHeader("STANDBY OK", C_CYAN);

      for (int i = 0; i < 6; i++) {
        updateCard(i, (wasteActive && i == 2) ? C_RED : C_WHITE);
      }
      drawWasted();

      Serial.printf("[%s] V:%.1f I:%.2f P:%.1f E:%.3f F:%.1f PF:%.2f Wasted:%.4fWh Cycles:%d\n",
                    wasteActive ? "WASTE" : (compOn ? "COMP" : "STBY"),
                    vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], wastedWh, cyclesThisHour);
    }
  }

  if (now - lastSave >= SAVE_INTERVAL_MS) {
    prefs.putFloat("waste", wastedWh);
    lastSave = now;
  }
}
