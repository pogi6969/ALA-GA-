// KALINGA v2: wearable health pendant on the Waveshare ESP32-S3 1.28-in round touch LCD board
// (GC9A01 display, CST816S touch, QMI8658 motion sensor, LiPo charger on board).
// Adds: touch screen, GPS location and safe zone, 4G calls and SMS (SIMCom A7670), steps and active
// minutes, symptom questions, plus the v1 vital-sign checks, fall/SOS/inactivity alerts, and pill carousel.
// KALINGA reminds, presents doses, checks, and alerts; it does not diagnose or treat.
// Libraries: GFX Library for Arduino (Arduino_GFX), CST816S (fbiego), TinyGPSPlus, Adafruit MLX90614,
// SparkFun MAX3010x, ESP32Servo, ArduinoJson 7. Board: ESP32S3 Dev Module, USB CDC On Boot: Enabled.
// Pin numbers follow the Waveshare wiki for this board; confirm them against your board before wiring.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <CST816S.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MLX90614.h>
#include <ESP32Servo.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ---------- Settings: edit before uploading ----------
// Wi-Fi, Firebase API key, database URL and device login live in secrets.h (not uploaded to GitHub).
// Copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"
const char *PATIENT = "lola1";

// ---------- Pins ----------
const int LCD_DC = 8, LCD_CS = 9, LCD_SCK = 10, LCD_MOSI = 11, LCD_RST = 14, LCD_BL = 2;  // on-board display
const int I2C_SDA = 6, I2C_SCL = 7, TP_INT = 5, TP_RST = 13;                              // touch + QMI8658 + sensors
const int PIN_BATT = 1;                    // on-board battery divider (x3)
const int PIN_SOS = 0;                     // BOOT button: hold 2 s for SOS
const int GPS_RX = 18, GPS_TX = 17;        // ATGM336H GPS on UART1
const int LTE_RX = 16, LTE_TX = 15;        // A7670 4G module on UART2
const int PIN_SERVO = 21, PIN_SERVO_PWR = 33, PIN_VIBE = 12;  // header pins; change if your board differs

// ---------- Limits ----------
const float TEMP_FEVER = 37.5, TEMP_HIGH = 39.0;
const int SPO2_LOW = 95, SPO2_URGENT = 92, PR_LOW = 60, PR_HIGH = 100, PR_URG_LOW = 40, PR_URG_HIGH = 130;
float TEMP_A = 1.00, TEMP_B = 2.30;
int SPO2_OFFSET = 0, PR_OFFSET = 0;
const float FREE_FALL_G = 0.5, IMPACT_G = 2.5, STILL_RANGE_G = 0.3, STEP_G = 1.15;
const int SLOT_ANGLE[5] = {0, 45, 90, 135, 180};
const uint16_t BG = 0x0000, FG = 0xFFFF, RED = 0xE8E4, GRN = 0x2589, YEL = 0xFE60, NAVY = 0x19AA, GRY = 0x7BEF;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, LCD_RST, 0, true);
CST816S touch(I2C_SDA, I2C_SCL, TP_RST, TP_INT);
TinyGPSPlus gps;
HardwareSerial &gpsSer = Serial1, &lte = Serial2;
Adafruit_MLX90614 mlx;
MAX30105 pox;
Servo carousel;
Preferences prefs;

struct Slot { char time[6]; char label[24]; bool enabled; } slots[4];
struct Contact { char name[16]; char phone[16]; } contacts[3];
int nContacts = 0, checkHour[2] = {8, 18}, inactivityHours = 3, doneDay[4] = {-1, -1, -1, -1}, checkDay[2] = {-1, -1};
double homeLat = 0, homeLng = 0; int homeRadius = 300; bool outsideSent = false, autoAnswer = true;
String idToken, refreshToken, callerId;
unsigned long tokenAt = 0, lastSync = 0, lastMove = 0, lastTouch = 0, lastStep = 0, lastGpsSend = 0;
long steps = 0, activeMin = 0, stepsThisMin = 0; int today = -1, minuteMark = -1;
bool inactivitySent = false, inCall = false, ringing = false;

// =============== Screen ===============
struct Btn { int x, y, r; const char *label; uint16_t color; };
void centered(const String &s, int y, int size, uint16_t c = FG) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setTextSize(size); gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(c); gfx->setCursor(120 - w / 2, y); gfx->print(s);
}
void drawBtn(const Btn &b) { gfx->fillCircle(b.x, b.y, b.r, b.color); centered(b.label, b.y - 6, 2, FG); }
bool hit(const Btn &b, int x, int y) { return sq(x - b.x) + sq(y - b.y) <= sq(b.r + 6); }
void screen(const String &a, const String &b = "", uint16_t c = NAVY) {
  gfx->fillScreen(c); centered(a, 80, 3); centered(b, 125, 2);
  digitalWrite(LCD_BL, HIGH); lastTouch = millis();
}
// Waits for a tap; returns false on timeout. Wakes the screen.
bool tap(int &x, int &y, unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) {
    serviceRadios();
    if (touch.available() && touch.data.gestureID == SINGLE_CLICK) {
      x = touch.data.x; y = touch.data.y; lastTouch = millis(); digitalWrite(LCD_BL, HIGH); return true;
    }
    delay(15);
  }
  return false;
}
int choose(const String &q, const Btn *b, int n, unsigned long ms) {
  gfx->fillScreen(NAVY); centered(q, 60, 2);
  for (int i = 0; i < n; i++) drawBtn(b[i]);
  int x, y; unsigned long t = millis();
  while (millis() - t < ms) {
    if (!tap(x, y, ms - (millis() - t))) break;
    for (int i = 0; i < n; i++) if (hit(b[i], x, y)) return i;
  }
  return -1;
}
bool askYesNo(const String &q) {
  Btn b[2] = {{70, 160, 38, "OO", RED}, {170, 160, 38, "HINDI", GRN}};
  return choose(q, b, 2, 30000) == 0;
}
int askPain() {
  int p = 0, x, y;
  Btn minus = {55, 165, 30, "-", GRY}, plus = {185, 165, 30, "+", GRY}, ok = {120, 195, 30, "OK", GRN};
  while (true) {
    gfx->fillScreen(NAVY); centered("Sakit 0-10", 45, 2); centered(String(p), 85, 5);
    drawBtn(minus); drawBtn(plus); drawBtn(ok);
    if (!tap(x, y, 30000)) return p;
    if (hit(minus, x, y)) p = max(0, p - 1); else if (hit(plus, x, y)) p = min(10, p + 1); else if (hit(ok, x, y)) return p;
  }
}
void vibrate(int times) { for (int i = 0; i < times; i++) { digitalWrite(PIN_VIBE, HIGH); delay(300); digitalWrite(PIN_VIBE, LOW); delay(200); } }

// =============== Motion (QMI8658 on I2C 0x6B) ===============
void imuWrite(uint8_t r, uint8_t v) { Wire.beginTransmission(0x6B); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
void imuBegin() { imuWrite(0x02, 0x40); imuWrite(0x03, 0x25); imuWrite(0x08, 0x01); }  // auto-increment; +-8 g; accel on
float accel(float *z = nullptr) {
  Wire.beginTransmission(0x6B); Wire.write(0x35); Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x6B, (uint8_t)6);
  int16_t ax = Wire.read() | Wire.read() << 8, ay = Wire.read() | Wire.read() << 8, az = Wire.read() | Wire.read() << 8;
  if (z) *z = az / 4096.0;
  return sqrt(sq(ax / 4096.0) + sq(ay / 4096.0) + sq(az / 4096.0));
}

// =============== Radios: GPS and 4G ===============
String lteLine;
String at(const String &cmd, unsigned long ms = 1500) {
  while (lte.available()) lte.read();
  lte.println(cmd);
  String r; unsigned long t = millis();
  while (millis() - t < ms) { while (lte.available()) r += (char)lte.read(); if (r.endsWith("OK\r\n") || r.indexOf("ERROR") >= 0) break; }
  return r;
}
void sms(const char *num, const String &text) {
  at("AT+CMGF=1");
  lte.printf("AT+CMGS=\"%s\"\r", num); delay(400);
  lte.print(text.substring(0, 150)); lte.write(26);
  String r; unsigned long t = millis();  // wait for +CMGS and OK
  while (millis() - t < 8000 && r.indexOf("OK") < 0 && r.indexOf("ERROR") < 0) while (lte.available()) r += (char)lte.read();
}
void serviceRadios() {
  while (gpsSer.available()) gps.encode(gpsSer.read());
  while (lte.available()) {
    char c = lte.read();
    if (c != '\n') { if (c != '\r') lteLine += c; continue; }
    if (lteLine == "RING") ringing = true;
    else if (lteLine.startsWith("+CLIP:")) { int a = lteLine.indexOf('"'); callerId = lteLine.substring(a + 1, lteLine.indexOf('"', a + 1)); }
    else if (lteLine.indexOf("NO CARRIER") >= 0 || lteLine.indexOf("VOICE CALL: END") >= 0) { inCall = ringing = false; }
    lteLine = "";
  }
}
String mapLink() {
  if (!gps.location.isValid()) return "location not available";
  return "https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
}
const char *nameOf(const String &num) {
  for (int i = 0; i < nContacts; i++) if (num.endsWith(String(contacts[i].phone).substring(3))) return contacts[i].name;
  return nullptr;
}
void inCallScreen(const String &who) {
  inCall = true; ringing = false;
  Btn end = {120, 175, 40, "BABA", RED};
  gfx->fillScreen(NAVY); centered("Kausap:", 60, 2); centered(who, 95, 2); drawBtn(end);
  int x, y;
  while (inCall) { if (tap(x, y, 500) && hit(end, x, y)) { at("AT+CHUP"); inCall = false; } }
  screen("Tapos na", "Call ended");
  delay(1500);
}
void callMenu() {
  if (!nContacts) { screen("Walang contact", "Add in the app"); delay(2000); return; }
  Btn b[4]; int n = 0;
  int ys[3] = {95, 145, 195};
  for (int i = 0; i < nContacts; i++) b[n++] = {120, ys[i], 24, contacts[i].name, GRN};
  int k = choose("Tawagan si:", b, n, 20000);
  if (k < 0) return;
  screen("Tumatawag...", contacts[k].name);
  at(String("ATD") + contacts[k].phone + ";", 3000);
  inCallScreen(contacts[k].name);
}
void handleIncoming() {
  if (!ringing) return;
  const char *who = nameOf(callerId);
  Btn b[2] = {{70, 165, 38, "SAGOT", GRN}, {170, 165, 38, "HINDI", RED}};
  gfx->fillScreen(NAVY); centered("May tumatawag", 55, 2); centered(who ? who : callerId, 90, 2);
  drawBtn(b[0]); drawBtn(b[1]); vibrate(2);
  unsigned long t = millis(); int x, y;
  while (ringing) {
    if (tap(x, y, 300)) {
      if (hit(b[0], x, y)) { at("ATA"); inCallScreen(who ? who : callerId); return; }
      if (hit(b[1], x, y)) { at("AT+CHUP"); ringing = false; break; }
    }
    if (autoAnswer && who && millis() - t > 15000) { at("ATA"); inCallScreen(who); return; }  // family can always reach Lola
  }
}

// =============== Firebase (REST) ===============
bool postJson(const String &url, const String &body, String &out, const char *type = "application/json") {
  WiFiClientSecure tls; tls.setInsecure();  // prototype only: add Google's root certificate before real use
  HTTPClient http;
  if (!http.begin(tls, url)) return false;
  http.addHeader("Content-Type", type);
  int code = http.POST(body); out = http.getString(); http.end();
  return code == 200;
}
bool signIn() {
  String out; bool ok;
  if (refreshToken.length()) {
    ok = postJson(String("https://securetoken.googleapis.com/v1/token?key=") + API_KEY,
                  "grant_type=refresh_token&refresh_token=" + refreshToken, out, "application/x-www-form-urlencoded");
  } else {
    JsonDocument q; q["email"] = DEVICE_EMAIL; q["password"] = DEVICE_PASS; q["returnSecureToken"] = true;
    String b; serializeJson(q, b);
    ok = postJson(String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + API_KEY, b, out);
  }
  if (!ok) { refreshToken = ""; return false; }
  JsonDocument r; deserializeJson(r, out);
  idToken = r["idToken"] | ""; if (!idToken.length()) idToken = r["id_token"] | "";
  refreshToken = r["refreshToken"] | ""; if (!refreshToken.length()) refreshToken = r["refresh_token"] | "";
  tokenAt = millis();
  return idToken.length() > 0;
}
bool db(const char *method, const String &path, const String &body, String *reply = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!idToken.length() || millis() - tokenAt > 50UL * 60 * 1000) if (!signIn()) return false;
  WiFiClientSecure tls; tls.setInsecure();
  HTTPClient http;
  if (!http.begin(tls, String(DB_URL) + "/patients/" + PATIENT + path + ".json?auth=" + idToken)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (reply) *reply = http.getString();
  http.end();
  return code == 200;
}
String qPath[12], qBody[12]; int queued = 0;
void send(const String &path, const String &body) {
  if (db("POST", path, body)) return;
  if (queued < 12) { qPath[queued] = path; qBody[queued++] = body; }
}
void flushQueue() {
  int kept = 0;
  for (int i = 0; i < queued; i++) if (!db("POST", qPath[i], qBody[i])) { qPath[kept] = qPath[i]; qBody[kept++] = qBody[i]; }
  queued = kept;
}
uint64_t nowMs() { return (uint64_t)time(nullptr) * 1000ULL; }
// Critical alerts also go by SMS with the GPS link, so they reach the family even without Wi-Fi.
void sendAlert(const char *type, const String &msg, bool critical) {
  JsonDocument d; d["ts"] = nowMs(); d["type"] = type; d["msg"] = msg; d["ack"] = false;
  if (gps.location.isValid()) { d["lat"] = gps.location.lat(); d["lng"] = gps.location.lng(); }
  String b; serializeJson(d, b); send("/alerts", b);
  if (critical) for (int i = 0; i < nContacts; i++) sms(contacts[i].phone, String("KALINGA ") + type + ": " + msg + " " + mapLink());
}

// =============== Sync with the app ===============
void loadPrefs() {
  prefs.begin("kalinga", true);
  for (int i = 0; i < 4; i++) {
    String t = prefs.getString(("t" + String(i)).c_str(), ""), l = prefs.getString(("l" + String(i)).c_str(), "Gamot");
    strlcpy(slots[i].time, t.c_str(), 6); strlcpy(slots[i].label, l.c_str(), 24); slots[i].enabled = t.length() == 5;
  }
  nContacts = prefs.getInt("nc", 0);
  for (int i = 0; i < nContacts; i++) {
    strlcpy(contacts[i].name, prefs.getString(("cn" + String(i)).c_str(), "").c_str(), 16);
    strlcpy(contacts[i].phone, prefs.getString(("cp" + String(i)).c_str(), "").c_str(), 16);
  }
  homeLat = prefs.getDouble("hla", 0); homeLng = prefs.getDouble("hln", 0); homeRadius = prefs.getInt("hr", 300);
  prefs.end();
}
void syncFromApp() {
  String r;
  JsonDocument d;
  if (db("GET", "/schedule", "", &r) && !deserializeJson(d, r)) {
    prefs.begin("kalinga", false);
    for (int i = 0; i < 4; i++) {
      JsonVariant s = d[String(i + 1)];
      prefs.putString(("t" + String(i)).c_str(), (s["enabled"] | false) ? String((const char *)(s["time"] | "")) : "");
      prefs.putString(("l" + String(i)).c_str(), String((const char *)(s["label"] | "Gamot")));
    }
    prefs.end();
  }
  if (db("GET", "/settings", "", &r) && !deserializeJson(d, r)) {
    checkHour[0] = d["checkHours"][0] | 8; checkHour[1] = d["checkHours"][1] | 18;
    inactivityHours = d["inactivityHours"] | 3; autoAnswer = d["autoAnswer"] | true;
    prefs.begin("kalinga", false);
    JsonArray cs = d["contacts"].as<JsonArray>();
    int n = 0;
    for (JsonVariant c : cs) if (n < 3 && (c["phone"] | "")[0]) {
      prefs.putString(("cn" + String(n)).c_str(), String((const char *)(c["name"] | "Family")));
      prefs.putString(("cp" + String(n)).c_str(), String((const char *)(c["phone"] | ""))); n++;
    }
    prefs.putInt("nc", n);
    prefs.putDouble("hla", d["home"]["lat"] | 0.0); prefs.putDouble("hln", d["home"]["lng"] | 0.0);
    prefs.putInt("hr", d["home"]["radius"] | 300);
    prefs.end();
  }
  loadPrefs();
  if (db("GET", "/commands", "", &r) && !deserializeJson(d, r) && !d.isNull()) {
    db("DELETE", "/commands", "");
    if (!d["ring"].isNull()) { screen("Nandito ako!", "I am here"); vibrate(6); }
    if (!d["check"].isNull()) vitalsCheck("app");
  }
  JsonDocument s;
  s["ts"] = nowMs(); s["battery"] = batteryPct(); s["rssi"] = WiFi.RSSI(); s["queued"] = queued;
  s["steps"] = steps; s["activeMin"] = activeMin; s["lte"] = at("AT+CSQ").indexOf("+CSQ") >= 0;
  String b; serializeJson(s, b); db("PUT", "/status", b);
  struct tm tm;
  if (getLocalTime(&tm, 10)) {  // daily activity history for the app's chart
    char dk[11]; strftime(dk, sizeof dk, "%Y-%m-%d", &tm);
    JsonDocument a; a["steps"] = steps; a["activeMin"] = activeMin;
    String ab; serializeJson(a, ab); db("PUT", String("/activity/") + dk, ab);
  }
  flushQueue();
}
int batteryPct() { float v = analogReadMilliVolts(PIN_BATT) * 3 / 1000.0; return constrain((int)((v - 3.4) / 0.8 * 100), 0, 100); }

// =============== Health ===============
void vitalsCheck(const char *source) {
  unsigned long t0 = millis();
  float tempC = NAN; int pr = -1, spo2 = -1;
  screen("Ilapit sa noo", "Pendant to forehead"); vibrate(1);
  unsigned long t = millis();
  while (isnan(tempC) && millis() - t < 60000) {
    if (mlx.readObjectTempC() > 32.0) {
      delay(300); float sum = 0;
      for (int i = 0; i < 5; i++) { sum += mlx.readObjectTempC(); delay(100); }
      tempC = TEMP_A * sum / 5 + TEMP_B;
    }
    delay(50);
  }
  screen("Daliri sa likod", "Finger on back pad"); vibrate(1);
  const int N = 100; static uint32_t ir[N], red[N];
  t = millis();
  while (spo2 < 0 && millis() - t < 60000) {
    pox.check();
    if (pox.getIR() < 50000) { delay(50); continue; }
    screen("Huwag gumalaw", "Hold still");
    for (int i = 0; i < N; i++) { while (!pox.available()) pox.check(); red[i] = pox.getRed(); ir[i] = pox.getIR(); pox.nextSample(); }
    int32_t s, hr; int8_t sOk, hrOk;
    maxim_heart_rate_and_oxygen_saturation(ir, N, red, &s, &sOk, &hr, &hrOk);
    if (sOk && hrOk && hr > 30 && hr < 220 && s > 70) { spo2 = min(100, (int)s + SPO2_OFFSET); pr = hr + PR_OFFSET; }
  }
  int pain = askPain();
  bool dizzy = askYesNo("Nahihilo?"), breath = askYesNo("Hirap huminga?"), chest = askYesNo("Masakit dibdib?");
  bool red_ = tempC >= TEMP_HIGH || (spo2 > 0 && spo2 < SPO2_URGENT) || (pr > 0 && (pr < PR_URG_LOW || pr > PR_URG_HIGH)) ||
              chest || breath || pain >= 7;
  bool yel = tempC >= TEMP_FEVER || (spo2 > 0 && spo2 < SPO2_LOW) || (pr > 0 && (pr < PR_LOW || pr > PR_HIGH)) || dizzy || pain >= 4;
  const char *level = red_ ? "RED" : yel ? "YELLOW" : "GREEN";
  screen(String(tempC, 1) + "C " + pr, String("SpO2 ") + spo2 + "%  " + level, red_ ? RED : yel ? YEL : GRN);
  JsonDocument d;
  d["ts"] = nowMs(); d["temp"] = isnan(tempC) ? 0 : round(tempC * 10) / 10.0; d["hr"] = pr; d["spo2"] = spo2;
  d["pain"] = pain; d["dizzy"] = dizzy; d["breath"] = breath; d["chest"] = chest;
  d["level"] = level; d["source"] = source; d["secs"] = (millis() - t0) / 1000;
  String b; serializeJson(d, b); db("PUT", "/latest", b); send("/vitals", b);
  if (red_) sendAlert("VITALS_RED", String(tempC, 1) + " C, PR " + pr + ", SpO2 " + spo2 + "%, pain " + pain +
                      (chest ? ", chest pain" : "") + (breath ? ", short of breath" : ""), true);
  delay(5000);
}
void moveCarousel(int slot) {
  digitalWrite(PIN_SERVO_PWR, HIGH); carousel.attach(PIN_SERVO, 500, 2400);
  carousel.write(SLOT_ANGLE[slot]); delay(700); carousel.detach(); digitalWrite(PIN_SERVO_PWR, LOW);
}
void presentDose(int i, const String &due) {
  moveCarousel(i + 1);
  Btn ok = {120, 175, 42, "NAINOM", GRN};
  unsigned long t = millis(), lastRemind = 0; bool taken = false, tilted = false; int x, y;
  while (millis() - t < 30UL * 60 * 1000 && !taken) {
    if (millis() - lastRemind > 120000) {
      gfx->fillScreen(NAVY); centered("Gamot na po", 50, 2); centered(slots[i].label, 85, 2); drawBtn(ok);
      vibrate(3); lastRemind = millis();
    }
    float z; accel(&z); if (z < -0.5) tilted = true;
    if (tap(x, y, 100) && hit(ok, x, y)) taken = true;
  }
  moveCarousel(0);
  const char *st = taken ? (millis() - t > 600000 ? "late" : "taken") : "missed";
  JsonDocument d; d["ts"] = nowMs(); d["slot"] = i + 1; d["label"] = slots[i].label; d["due"] = due; d["status"] = st; d["tilted"] = tilted;
  String b; serializeJson(d, b); send("/doses", b);
  screen(taken ? "Salamat!" : "Hindi nainom", taken ? "Dose recorded" : "Family notified", taken ? GRN : RED);
  if (!taken) sendAlert("MISSED_DOSE", String("Missed ") + slots[i].label + " due " + due + ".", false);
}

// =============== Safety ===============
void countSteps(float g) {
  if (g > STEP_G && millis() - lastStep > 300) { steps++; stepsThisMin++; lastStep = millis(); }
  if (fabs(g - 1.0) > 0.15) { lastMove = millis(); inactivitySent = false; }
}
void checkFall(float g) {
  if (g >= FREE_FALL_G) return;
  float peak = 0;
  for (int i = 0; i < 50; i++) { peak = max(peak, accel()); delay(20); }
  if (peak < IMPACT_G) return;
  delay(1000);
  float lo = 9, hi = 0;
  for (int i = 0; i < 100; i++) { float a = accel(); lo = min(lo, a); hi = max(hi, a); delay(20); }
  if (hi - lo > STILL_RANGE_G) return;
  Btn ok = {120, 170, 45, "OK AKO", GRN};
  gfx->fillScreen(RED); centered("Nahulog ka ba?", 60, 2); centered("Tap if you are OK", 95, 1); drawBtn(ok);
  unsigned long t = millis(); int x, y;
  while (millis() - t < 10000) {
    digitalWrite(PIN_VIBE, (millis() / 250) % 2);
    if (tap(x, y, 50) && hit(ok, x, y)) { digitalWrite(PIN_VIBE, LOW); screen("OK", "Cancelled", GRN); return; }
  }
  digitalWrite(PIN_VIBE, LOW);
  screen("FALL ALERT", "Calling family", RED);
  sendAlert("FALL", "Possible fall, peak " + String(peak, 1) + " g, no answer in 10 s.", true);
  if (nContacts) { at(String("ATD") + contacts[0].phone + ";", 3000); inCallScreen(contacts[0].name); }
}
void checkZone() {
  if (!gps.location.isValid() || homeLat == 0) return;
  double dist = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), homeLat, homeLng);
  if (dist > homeRadius && !outsideSent) { outsideSent = true; sendAlert("LEFT_SAFE_ZONE", String((int)dist) + " m from home.", true); }
  if (dist < homeRadius * 0.8) outsideSent = false;
  if (millis() - lastGpsSend > 300000) {
    lastGpsSend = millis();
    JsonDocument d; d["ts"] = nowMs(); d["lat"] = gps.location.lat(); d["lng"] = gps.location.lng();
    d["sats"] = gps.satellites.value(); d["home"] = dist <= homeRadius;
    String b; serializeJson(d, b); db("PUT", "/location", b);
  }
}

// =============== Home screen and clock ===============
Btn bCheck = {62, 175, 32, "CHECK", NAVY}, bCall = {120, 200, 32, "CALL", GRN}, bSos = {178, 175, 32, "SOS", RED};
void homeScreen() {
  struct tm tm; char hm[6] = "--:--";
  if (getLocalTime(&tm, 10)) strftime(hm, sizeof hm, "%H:%M", &tm);
  gfx->fillScreen(BG);
  gfx->fillCircle(95, 38, 6, FG); gfx->fillCircle(145, 38, 6, FG);  // the two-dot face
  centered(hm, 62, 4);
  String next = "";
  for (int i = 0; i < 4; i++) if (slots[i].enabled && strcmp(slots[i].time, hm) > 0) { next = slots[i].time; break; }
  centered(next.length() ? "Gamot " + next : "KALINGA", 105, 2, YEL);
  centered(String(batteryPct()) + "%  " + steps + " steps", 132, 1, GRY);
  drawBtn(bCheck); drawBtn(bCall); drawBtn(bSos);
}
void checkClock() {
  struct tm tm;
  if (!getLocalTime(&tm, 10)) return;
  char hm[6]; strftime(hm, sizeof hm, "%H:%M", &tm);
  if (today != tm.tm_yday) { today = tm.tm_yday; steps = activeMin = 0; }
  for (int i = 0; i < 4; i++) if (slots[i].enabled && !strcmp(hm, slots[i].time) && doneDay[i] != tm.tm_yday) { doneDay[i] = tm.tm_yday; presentDose(i, hm); homeScreen(); }
  for (int k = 0; k < 2; k++) if (tm.tm_hour == checkHour[k] && tm.tm_min == 0 && checkDay[k] != tm.tm_yday) { checkDay[k] = tm.tm_yday; vitalsCheck("schedule"); homeScreen(); }
  if (tm.tm_min != minuteMark) {
    minuteMark = tm.tm_min;
    if (stepsThisMin >= 30) activeMin++;
    stepsThisMin = 0;
    if (millis() - lastTouch > 30000) { digitalWrite(LCD_BL, LOW); } else homeScreen();
  }
  if (tm.tm_hour >= 7 && tm.tm_hour < 21 && !inactivitySent && millis() - lastMove > (unsigned long)inactivityHours * 3600000UL) {
    inactivitySent = true; sendAlert("INACTIVE", String("No movement for ") + inactivityHours + " hours.", false);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SOS, INPUT_PULLUP); pinMode(PIN_VIBE, OUTPUT); pinMode(PIN_SERVO_PWR, OUTPUT); pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  gfx->begin(); gfx->fillScreen(NAVY);
  touch.begin();
  Wire.setClock(100000);  // MLX90614 needs 100 kHz
  imuBegin(); mlx.begin(); pox.begin(Wire, I2C_SPEED_STANDARD); pox.setup(60, 4, 2, 100, 411, 4096);
  gpsSer.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  lte.begin(115200, SERIAL_8N1, LTE_RX, LTE_TX);
  at("AT"); at("ATE0"); at("AT+CLIP=1"); at("AT+CMGF=1"); at("AT+CLVL=5");
  ESP32PWM::allocateTimer(0); moveCarousel(0);
  loadPrefs();
  screen("KALINGA", "Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  lastMove = millis(); syncFromApp(); homeScreen();
}

void loop() {
  serviceRadios();
  handleIncoming();
  float g = accel();
  countSteps(g);
  checkFall(g);
  if (digitalRead(PIN_SOS) == LOW) {
    unsigned long t = millis();
    while (digitalRead(PIN_SOS) == LOW && millis() - t < 2000) delay(10);
    if (millis() - t >= 2000) { screen("SOS", "Family notified", RED); vibrate(2); sendAlert("SOS", "SOS button held.", true); }
  }
  if (touch.available() && touch.data.gestureID == SINGLE_CLICK) {
    int x = touch.data.x, y = touch.data.y; lastTouch = millis();
    if (digitalRead(LCD_BL) == LOW) { digitalWrite(LCD_BL, HIGH); homeScreen(); }
    else if (hit(bCheck, x, y)) { vitalsCheck("button"); homeScreen(); }
    else if (hit(bCall, x, y)) { callMenu(); homeScreen(); }
    else if (hit(bSos, x, y)) { screen("SOS?", "Hold BOOT 2 s or tap again", RED); int a, b;
      if (tap(a, b, 5000) && hit(bSos, a, b)) { sendAlert("SOS", "SOS tapped on screen.", true); screen("SOS", "Family notified", RED); delay(2000); }
      homeScreen(); }
  }
  checkZone();
  checkClock();
  if (millis() - lastSync > 60000) { lastSync = millis(); syncFromApp(); }
  delay(20);
}
