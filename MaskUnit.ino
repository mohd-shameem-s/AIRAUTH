// MaskUnit.ino (ESP8266-2) - with Twilio SMS Alert on 3 Consecutive Fails

#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_ADXL345_U.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ---------- WiFi Config ----------
const char* ssid = "ABCDEFGH"; // Replace with your Wifi Username
const char* password = "12345678"; // Replace with your Wifi Password

// ---------------- UDP Config ----------------
WiFiUDP Udp;
const unsigned int localUdpPort = 4210;
const char* maskOnMsg = "MASK_ON";
const char* authOkMsg = "AUTH_OK";
const char* authFailMsg = "AUTH_FAIL";

// ---------- Twilio Config ----------
const char* accountSID = "xxx"; // Replace with your Twilio SID
const char* authToken  = "yyy"; // Replace with your Twilio Auth Token
const char* twilioFrom = "zzz"; // Your Twilio phone number with country code prefix (i.e) +91 zzz
const char* twilioTo   = "ttt"; // Your verified phone number with country code prefix (i.e) +91 ttt


// ---------------- Sensor Pins ----------------
const int blueLed = D3;
const int breathPin = A0;

// ---------------- Thresholds ----------------
const float tiltThreshold = 4.0;
const int breathPeakThreshold = 400;
const unsigned long gestureTimeout = 5000;
const unsigned long patternTimeout = 15000;
const unsigned long breatheWindowMs = 4000;

// ---------------- Accelerometer ----------------
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);

// ---------------- Print Rate Control ----------------
unsigned long lastPrintTime = 0;
const unsigned long printInterval = 400;

// ---------------- Authentication Fail Tracking ----------------
int failCount = 0;
const int maxFailsBeforeAlert = 3;

// ---------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(blueLed, OUTPUT);
  digitalWrite(blueLed, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  Udp.begin(localUdpPort);
  Serial.printf("UDP listening on %d\n", localUdpPort);

  if (!accel.begin()) Serial.println("ADXL345 not detected!");
  else accel.setRange(ADXL345_RANGE_16_G);
}

// ---------------- Status Printer ----------------
bool printStatusIfDue(const char* state) {
  if (millis() - lastPrintTime > printInterval) {
    Serial.printf("Current State: %-8s | Breath: %d\n", state, analogRead(breathPin));
    lastPrintTime = millis();
    return true;
  }
  return false;
}

// ---------------- Tilt Checks ----------------
bool isTiltRight() { sensors_event_t e; accel.getEvent(&e); printStatusIfDue("RIGHT"); return (e.acceleration.x < -tiltThreshold); }
bool isTiltLeft()  { sensors_event_t e; accel.getEvent(&e); printStatusIfDue("LEFT");  return (e.acceleration.x > tiltThreshold); }
bool isStraight()  { sensors_event_t e; accel.getEvent(&e); printStatusIfDue("STRAIGHT"); return (fabs(e.acceleration.x) < 1.0); }

// ---------------- UDP Sender ----------------
void sendUdpMsg(const char* msg) {
  Udp.beginPacket("255.255.255.255", localUdpPort);
  Udp.write(msg);
  Udp.endPacket();
  Serial.printf("Sent UDP: %s\n", msg);
}

// ---------------- Breath Detection ----------------
bool detectTwoBreaths() {
  Serial.println("Waiting for 2 breaths...");
  unsigned long start = millis();
  int count = 0, lastVal = analogRead(breathPin);

  while (millis() - start < breatheWindowMs) {
    int val = analogRead(breathPin);
    if (millis() - lastPrintTime > printInterval) {
      Serial.printf("Breath Capacity: %d\n", val);
      lastPrintTime = millis();
    }
    if (val > breathPeakThreshold && lastVal <= breathPeakThreshold) {
      count++;
      Serial.printf("Breath #%d detected (val=%d)\n", count, val);
      delay(800);
    }
    lastVal = val;
    if (count >= 2) return true;
    delay(100);
  }
  return false;
}

// ---------------- Sequence Functions ----------------
bool performTiltRight() { Serial.println(">> Expecting Tilt RIGHT"); unsigned long t = millis(); while (millis() - t < gestureTimeout) { if (isTiltRight()) return true; delay(100);} return false; }
bool performTiltLeft()  { Serial.println(">> Expecting Tilt LEFT");  unsigned long t = millis(); while (millis() - t < gestureTimeout) { if (isTiltLeft())  return true; delay(100);} return false; }
bool performStraight(const char* l) { Serial.printf(">> Expecting STRAIGHT (%s)\n", l); unsigned long t = millis(); while (millis() - t < gestureTimeout) { if (isStraight()) return true; delay(100);} return false; }
bool performBreaths(const char* l)  { Serial.printf(">> Detecting 2 Breaths (%s)\n", l); return detectTwoBreaths(); }

// ---------------- Twilio SMS Function ----------------
void sendTwilioAlert(const char* reason) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot send alert.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Skip SSL certificate verification

  HTTPClient http;
  String url = "https://api.twilio.com/2010-04-01/Accounts/" + String(twilioAccountSID) + "/Messages.json";

  http.begin(client, url);  // ✅ Updated for ESP8266 Core 3.x compatibility
  http.setAuthorization(twilioAccountSID, twilioAuthToken);

  String body = "To=" + String(twilioToNumber) +
                "&From=" + String(twilioFromNumber) +
                "&Body=⚠️ Mask Auth Alert: 3 Failed Attempts Detected. Reason: " + String(reason);

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST(body);
  Serial.printf("Twilio SMS sent (HTTP %d)\n", code);
  if (code > 0) Serial.println(http.getString());
  http.end();
}

// ---------------- Authentication Handlers ----------------
void authFail(const char* reason) {
  Serial.printf("❌ AUTH FAIL: %s\n", reason);
  sendUdpMsg(authFailMsg);
  digitalWrite(blueLed, LOW);
  failCount++;

  if (failCount >= maxFailsBeforeAlert) {
    Serial.println("🚨 3 Consecutive Fails — Sending Twilio Alert...");
    sendTwilioAlert(reason);
    failCount = 0; // reset after alert
  }
}

void authSuccess() {
  Serial.println("✅ AUTH SUCCESS — Pattern Matched!");
  sendUdpMsg(authOkMsg);
  digitalWrite(blueLed, LOW);
  failCount = 0; // reset counter on success
}

// ---------------- Sequence Start ----------------
void startAuthSequence() {
  digitalWrite(blueLed, HIGH);
  Serial.println("=== Authentication Sequence Started ===");
  unsigned long seqStart = millis();

  if (!performTiltRight()) return authFail("Tilt Right Missing");
  if (!performBreaths("A")) return authFail("Breath A failed");
  if (!performStraight("A")) return authFail("Straight A failed");
  if (!performTiltLeft()) return authFail("Tilt Left Missing");
  if (!performBreaths("B")) return authFail("Breath B failed");
  if (!performStraight("B")) return authFail("Straight B failed");

  if (millis() - seqStart > patternTimeout) return authFail("Pattern Timeout");
  authSuccess();
}

// ---------------- Loop ----------------
void loop() {
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    char incoming[120];
    int len = Udp.read(incoming, 119);
    if (len > 0) incoming[len] = 0;
    if (String(incoming) == maskOnMsg) startAuthSequence();
  }
}
