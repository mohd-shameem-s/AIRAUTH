/* DoorUnit_IR.ino - Stable Version (Detection + OLED + Lockout)
   NodeMCU ESP8266-1
   IR sensor for presence detection
   OLED + Servo door control + Twilio SMS alert
   New: 3 failed attempts → 30s lockout with countdown
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Servo.h>
#include <WiFiClientSecure.h>

// ---------- OLED Config ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- WiFi Credentials ----------
const char* ssid = "ABCDEFGH"; // Replace with your Wifi Username
const char* password = "12345678"; // Repace with your Wifi Password

// ---------- Twilio Credentials ----------
const char* accountSID = "xxx"; // Replace with your Twilio SID
const char* authToken  = "yyy"; // Replace with your Twilio Auth Token
const char* twilioFrom = "zzz"; // Your Twilio phone number with country code prefix (i.e) +91 zzz
const char* twilioTo   = "ttt"; // Your verified phone number with country code prefix (i.e) +91 ttt
const char* twilioHost = "api.twilio.com";

// ---------- UDP ----------
WiFiUDP Udp;
const unsigned int localUdpPort = 4210;
const char* maskOnMsg = "MASK_ON";
const char* authOkMsg = "AUTH_OK";
const char* authFailMsg = "AUTH_FAIL";

// ---------- Hardware Pins ----------
const int irPin = D5;
const int servoPin = D7;
const int buzzerPin = D3;
const int greenLed = D8;
const int redLed = D4;
const int yellowLed = D0;

Servo doorServo;

// ---------- Timing & Flags ----------
bool awaitingAuth = false;
unsigned long authStart = 0;
const unsigned long authTimeout = 20000;
const unsigned long maskOnResend = 3000;
unsigned long lastMaskOnSent = 0;
const unsigned long doorOpenDuration = 15000;
bool personDetected = false;

// Debounce stability filter
unsigned long lastDetectionChange = 0;
bool lastIrState = false;
const unsigned long debounceTime = 500;

// ---------- Lockout Feature ----------
int failedAuthCount = 0;
bool lockoutActive = false;
unsigned long lockoutStart = 0;
const unsigned long lockoutDuration = 30000; // 30 seconds

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(irPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(greenLed, OUTPUT);
  pinMode(redLed, OUTPUT);
  pinMode(yellowLed, OUTPUT);

  doorServo.attach(servoPin);
  doorServo.write(0); // Door closed

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed!");
  }
  displayIdle();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  Udp.begin(localUdpPort);
}

// ---------- OLED Helper ----------
void displayTwoLines(const char* line1, const char* line2) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 10);
  display.println(line1);
  display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 35);
  display.println(line2);
  display.display();
}

void centerText(String text, int textSize, int y) {
  display.clearDisplay();
  display.setTextSize(textSize);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (display.width() - w) / 2;
  display.setCursor(x, y);
  display.print(text);
  display.display();
}

void displayIdle()      { centerText("AIRAUTH", 2, 25); }
void displayDetected()  { displayTwoLines("Person", "Detected"); }
void displayGranted()   { displayTwoLines("ACCESS", "GRANTED"); }
void displayDenied()    { displayTwoLines("ACCESS", "DENIED"); }
void displayLockout(int secondsLeft) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 10);
  display.print("LOCKOUT");
  display.setCursor(50, 35);
  display.print(secondsLeft);
  display.display();
}

// ---------- Twilio ----------
void sendTwilioSMS(const String &body) {
  WiFiClientSecure client;
  client.setInsecure();
  String url = "/2010-04-01/Accounts/" + String(accountSID) + "/Messages.json";
  if (!client.connect(twilioHost, 443)) { Serial.println("Twilio connection failed"); return; }
  String postData = "From=" + urlEncode(twilioFrom) + "&To=" + urlEncode(twilioTo) + "&Body=" + urlEncode(body);
  String auth64 = base64Encode(String(accountSID) + ":" + authToken);
  client.printf("POST %s HTTP/1.1\r\nHost: %s\r\n", url.c_str(), twilioHost);
  client.printf("Authorization: Basic %s\r\n", auth64.c_str());
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.printf("Content-Length: %d\r\n\r\n", postData.length());
  client.print(postData);
  delay(1000);
  client.stop();
  Serial.println("Twilio SMS sent.");
}

// ---------- Utility ----------
String urlEncode(const String &s) {
  String r; char c; char buf[4];
  for (int i = 0; i < s.length(); i++) {
    c = s.charAt(i);
    if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') r+=c;
    else if (c==' ') r+='+'; 
    else { sprintf(buf,"%%%02X",(unsigned char)c); r+=buf; }
  }
  return r;
}

String base64Encode(const String &input) {
  const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded = "";
  int i = 0; unsigned char char_array_3[3]; unsigned char char_array_4[4];
  int input_len = input.length();
  const unsigned char *bytes = (const unsigned char *)input.c_str();
  while (input_len--) {
    char_array_3[i++] = *(bytes++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;
      for (i = 0; i < 4; i++) encoded += base64_chars[char_array_4[i]]; i = 0;
    }
  }
  if (i) {
    for (int j = i; j < 3; j++) char_array_3[j] = '\0';
    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;
    for (int j = 0; j < i + 1; j++) encoded += base64_chars[char_array_4[j]];
    while ((i++ < 3)) encoded += '=';
  }
  return encoded;
}

// ---------- Main Loop ----------
void loop() {
  // ---------- LOCKOUT HANDLING ----------
  if (lockoutActive) {
    unsigned long elapsed = millis() - lockoutStart;
    if (elapsed < lockoutDuration) {
      int secondsLeft = (lockoutDuration - elapsed) / 1000;
      displayLockout(secondsLeft);
      delay(1000);
      return; // skip rest of loop during lockout
    } else {
      lockoutActive = false;
      failedAuthCount = 0;
      Serial.println("LOCKOUT ENDED");
      displayIdle();
    }
  }

  // ---------- IR Detection ----------
  bool irState = (digitalRead(irPin) == LOW);
  if (irState != lastIrState) { lastDetectionChange = millis(); lastIrState = irState; }
  if (millis() - lastDetectionChange > debounceTime) {
    if (irState && !personDetected) {
      personDetected = true;
      Serial.println("Person detected!");
      displayDetected();
      awaitingAuth = true;
      authStart = millis();
      lastMaskOnSent = 0;
      digitalWrite(yellowLed, HIGH);
    } else if (!irState && personDetected && !awaitingAuth) {
      personDetected = false;
      Serial.println("Person left, returning to idle");
      displayIdle();
    }
  }

  // ---------- Send MASK_ON ----------
  if (awaitingAuth && millis() - lastMaskOnSent > maskOnResend) {
    Udp.beginPacket("255.255.255.255", localUdpPort);
    Udp.write(maskOnMsg);
    Udp.endPacket();
    lastMaskOnSent = millis();
    Serial.println("MASK_ON sent");
  }

  // ---------- Handle UDP ----------
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    char buf[64];
    int len = Udp.read(buf, 63);
    buf[len] = 0;
    String msg(buf);
    Serial.print("Got UDP: "); Serial.println(msg);

    if (lockoutActive) { 
      Serial.println("System in lockout, ignoring auth"); 
      tone(buzzerPin, 1000, 100); 
      return; 
    }

    if (msg == authOkMsg) {
      Serial.println("ACCESS GRANTED");
      failedAuthCount = 0;
      digitalWrite(yellowLed, LOW);
      digitalWrite(greenLed, HIGH);
      tone(buzzerPin, 2000, 200);
      displayGranted();

      doorServo.write(90);
      delay(doorOpenDuration);
      doorServo.write(0);

      digitalWrite(greenLed, LOW);
      displayIdle();
      awaitingAuth = false;
      personDetected = false;
    } 
    else if (msg == authFailMsg) {
      Serial.println("ACCESS DENIED");
      failedAuthCount++;
      digitalWrite(yellowLed, LOW);
      digitalWrite(redLed, HIGH);
      tone(buzzerPin, 800, 400);
      displayDenied();
      sendTwilioSMS("ALERT: Unauthorized authentication attempt detected at door.");
      delay(2000);
      digitalWrite(redLed, LOW);
      displayIdle();
      awaitingAuth = false;
      personDetected = false;

      // Lockout trigger
      if (failedAuthCount >= 3) {
        lockoutActive = true;
        lockoutStart = millis();
        Serial.println("LOCKOUT STARTED: 30s");
      }
    }
  }

  // ---------- Auth timeout ----------
  if (awaitingAuth && millis() - authStart > authTimeout) {
    Serial.println("Auth timeout");
    digitalWrite(yellowLed, LOW);
    awaitingAuth = false;
    displayIdle();
  }

  delay(50);
}