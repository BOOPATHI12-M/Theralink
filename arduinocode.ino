// =====================================================
//   SMART ASSISTIVE SYSTEM (ALL-IN-ONE)
//   MODE 0 : VOICE CONTROL
//   MODE 1 : IR GESTURE CONTROL
//   MODE 2 : BREATH / SOUND CONTROL (MAX4466)
//   WiFi + HTTP → sends data to Flask server
//   MacroDroid call via HTTP → http://10.175.4.208:8080/bm
// =====================================================

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DFRobot_DF2301Q.h"

// ================= WiFi / SERVER =================
const char* WIFI_SSID     = "bm";
const char* WIFI_PASSWORD = "12345678";
const char* SERVER_IP     = "10.175.4.138";
const int   SERVER_PORT   = 5000;

// MacroDroid HTTP trigger URL
const char* MACRODROID_URL = "http://10.175.4.208:8080/bm";

unsigned long lastSend = 0;
const unsigned long sendInterval = 3000;
unsigned long lastPoll = 0;
const unsigned long pollInterval = 2000;

// ================= PIN DEFINITIONS =================
#define BUTTON_PIN     4
#define BUTTON_CANCEL  5

#define IR_SENSOR 32
#define MIC_PIN   33

#define RELAY1 16
#define RELAY2 17
#define RELAY3 18

#define GSM_RX 26
#define GSM_TX 27

#define SAMPLE_WINDOW 50

// ================= OBJECTS =================
DFRobot_DF2301Q_I2C voice;
HardwareSerial gsmSerial(2);

// ================= MODE =================
int mode = 0;
bool lastState = HIGH;
unsigned long lastPress = 0;

// ================= RELAY STATES =================
bool relay1State = false;
bool relay2State = false;

// ================= VOICE COMMAND TRACKING =================
uint8_t lastVoiceCmdID = 0;

// =================================================
//                BREATH VARIABLES
// =================================================
int startThreshold;
int stopThreshold;

bool breathing = false;
int breathCount = 0;

unsigned long lastBreathTime = 0;
unsigned long lastSerialPrint = 0;

// =================================================
//                IR VARIABLES
// =================================================
int countInput = 0;
bool wakeup = false;
unsigned long startTime = 0;
const unsigned long interval = 2000;

bool lastIRState = LOW;
unsigned long lastIRTrigger = 0;
const unsigned long irDelay = 400;

// =================================================
//                RELAY3 TIMER
// =================================================
bool relay3Active = false;
unsigned long relay3Start = 0;
const unsigned long relay3Duration = 5000;  // 5 seconds

// --- Cancel window (IR mode) ---
bool waitingForCancel = false;
unsigned long cancelStart = 0;
const unsigned long cancelWindow = 7000;

// --- Breath mode emergency ---
bool breathEmergencyActive = false;      // relay3 is ON due to breath >=3
bool breathWaitingCall = false;          // waiting 5s before calling macrodroid
unsigned long breathRelay3Start = 0;     // when relay3 turned on in breath mode
bool breathCancelled = false;            // cancelled by button press

// =================================================
//              WiFi CONNECT
// =================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected  IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi FAILED — running offline");
  }
}

// =================================================
//        MACRODROID HTTP TRIGGER
// =================================================
void triggerMacroDroid() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MacroDroid] WiFi not connected, skipping call.");
    return;
  }

  Serial.println("[MacroDroid] Triggering call via HTTP...");
  HTTPClient http;
  http.begin(MACRODROID_URL);
  int code = http.GET();
  if (code > 0) {
    Serial.printf("[MacroDroid] Response code: %d\n", code);
  } else {
    Serial.printf("[MacroDroid] Failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// =================================================
//        SEND DATA TO FLASK SERVER
// =================================================
void sendToServer() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/update";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String gsmSt = "idle";
  int cancelRemain = 0;
  if (waitingForCancel) {
    unsigned long elapsed = millis() - cancelStart;
    if (elapsed < cancelWindow) {
      gsmSt = "waiting_cancel";
      cancelRemain = (int)((cancelWindow - elapsed) / 1000);
    }
  }
  if (breathWaitingCall) {
    gsmSt = "breath_emergency";
    unsigned long elapsed = millis() - breathRelay3Start;
    if (elapsed < relay3Duration)
      cancelRemain = (int)((relay3Duration - elapsed) / 1000);
  }

  StaticJsonDocument<384> doc;
  doc["mode"]             = mode;
  doc["relay1"]           = relay1State;
  doc["relay2"]           = relay2State;
  doc["relay3"]           = relay3Active || breathEmergencyActive || (digitalRead(RELAY3) == LOW);
  doc["breath_level"]     = (mode == 2) ? smoothBreath() : 0;
  doc["breath_count"]     = breathCount;
  doc["ir_count"]         = countInput;
  doc["gsm_status"]       = gsmSt;
  doc["cancel_remaining"] = cancelRemain;
  doc["last_voice_cmd"]   = lastVoiceCmdID;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("[HTTP] POST → %d\n", code);
  } else {
    Serial.printf("[HTTP] POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// =================================================
// CHECK SERVER FOR RELAY COMMANDS FROM DASHBOARD
// =================================================
void checkServerCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/relay/status";
  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      if (!doc["relay1"].isNull()) {
        bool st = doc["relay1"].as<bool>();
        relay1State = st;
        digitalWrite(RELAY1, st ? LOW : HIGH);
        Serial.printf("[SERVER] Fan → %s\n", st ? "ON" : "OFF");
      }
      if (!doc["relay2"].isNull()) {
        bool st = doc["relay2"].as<bool>();
        relay2State = st;
        digitalWrite(RELAY2, st ? LOW : HIGH);
        Serial.printf("[SERVER] Light → %s\n", st ? "ON" : "OFF");
      }
      if (!doc["relay3"].isNull()) {
        bool st = doc["relay3"].as<bool>();
        if (st) {
          digitalWrite(RELAY3, LOW);
          relay3Active = true;
          relay3Start = millis();
        } else {
          digitalWrite(RELAY3, HIGH);
          relay3Active = false;
          waitingForCancel = false;
        }
        Serial.printf("[SERVER] Emergency → %s\n", st ? "ON" : "OFF");
      }
    }
  }
  http.end();
}

// =================================================
// GSM CALL (legacy)
// =================================================
void callGSM() {
  Serial.println("Calling via GSM...");
  gsmSerial.println("AT");
  delay(1000);
  gsmSerial.println("ATD+919944145515;");
}

// =================================================
// MODE SWITCH
// =================================================
void checkSwitch() {
  bool current = digitalRead(BUTTON_PIN);
  if (lastState == HIGH && current == LOW) {
    if (millis() - lastPress > 250) {
      mode++;
      if (mode > 2) mode = 0;
      Serial.print("MODE → ");
      if (mode == 0) Serial.println("VOICE");
      if (mode == 1) Serial.println("IR GESTURE");
      if (mode == 2) Serial.println("BREATH/SOUND");
      lastPress = millis();
    }
  }
  lastState = current;
}

// =================================================
// SETUP
// =================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(BUTTON_PIN,    INPUT_PULLUP);
  pinMode(BUTTON_CANCEL, INPUT_PULLUP);
  pinMode(IR_SENSOR,     INPUT);

  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);

  digitalWrite(RELAY1, HIGH);
  digitalWrite(RELAY2, HIGH);
  digitalWrite(RELAY3, HIGH);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  Wire.begin(21, 22);

  if (!voice.begin()) {
    Serial.println("Voice module not detected!");
    while (1);
  }

  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);

  connectWiFi();
  autoCalibrate();

  Serial.println("✅ System Ready");
  Serial.print("Server: http://");
  Serial.print(SERVER_IP);
  Serial.print(":");
  Serial.println(SERVER_PORT);
  Serial.print("MacroDroid URL: ");
  Serial.println(MACRODROID_URL);
}

// =================================================
// LOOP
// =================================================
void loop() {
  checkSwitch();

  // ================= MODE 0 : VOICE =================
  if (mode == 0) {
    uint8_t cmdID = voice.getCMDID();
    if (cmdID != 0) {
      lastVoiceCmdID = cmdID;
      Serial.println("\n🎤 VOICE COMMAND RECEIVED");
      Serial.print("Command ID : ");
      Serial.println(cmdID);

      if      (cmdID == 5)  { digitalWrite(RELAY1, LOW);  relay1State = true;  Serial.println("[VOICE] Fan ON");  }
      else if (cmdID == 6)  { digitalWrite(RELAY1, HIGH); relay1State = false; Serial.println("[VOICE] Fan OFF"); }
      else if (cmdID == 7)  { digitalWrite(RELAY2, LOW);  relay2State = true;  Serial.println("[VOICE] Light ON");  }
      else if (cmdID == 8)  { digitalWrite(RELAY2, HIGH); relay2State = false; Serial.println("[VOICE] Light OFF"); }

      else if (cmdID == 11) {
        digitalWrite(RELAY3, LOW);
        relay3Active = true;
        relay3Start = millis();
        Serial.println("[VOICE] Emergency ON → Triggering MacroDroid call!");
        triggerMacroDroid();
      }
      else if (cmdID == 12) {
        digitalWrite(RELAY3, HIGH);
        relay3Active = false;
        Serial.println("[VOICE] Emergency OFF");
      }

      sendToServer();
      lastSend = millis();
    }
  }

  // ================= MODE 1 : IR =================
  else if (mode == 1) {
    bool irState = digitalRead(IR_SENSOR);

    if (irState == HIGH && lastIRState == LOW &&
        millis() - lastIRTrigger > irDelay) {

      lastIRTrigger = millis();

      if (!wakeup) {
        wakeup = true;
        startTime = millis();
        countInput = 1;
      } else {
        countInput++;
      }

      // ---- Print current IR count and relay status ----
      Serial.print("[IR] Count: ");
      Serial.print(countInput);
      Serial.print("  |  Fan: ");
      Serial.print(relay1State ? "ON" : "OFF");
      Serial.print("  Light: ");
      Serial.print(relay2State ? "ON" : "OFF");
      Serial.print("  Emergency: ");
      Serial.println((relay3Active || breathEmergencyActive) ? "ON" : "OFF");
    }

    lastIRState = irState;

    if (wakeup && millis() - startTime >= interval) {
      Serial.print("\n[IR] Final Count = ");
      Serial.println(countInput);

      if (countInput == 1) {
        toggleFan();
        Serial.print("[IR] 1 gesture → Fan toggled → Now: ");
        Serial.println(relay1State ? "ON" : "OFF");
      }
      else if (countInput == 2) {
        toggleLight();
        Serial.print("[IR] 2 gestures → Light toggled → Now: ");
        Serial.println(relay2State ? "ON" : "OFF");
      }
      else if (countInput >= 3) {
        Serial.println("[IR] 3+ gestures → Emergency Relay3 ON");
        digitalWrite(RELAY3, LOW);
        relay3Active = true;
        relay3Start = millis();

        waitingForCancel = true;
        cancelStart = millis();
        Serial.printf("[IR] Cancel window open for %lu seconds. Press BUTTON_CANCEL to abort.\n", cancelWindow / 1000);
      }

      wakeup = false;
      countInput = 0;
    }

    relay3Manager();
  }

  // ================= MODE 2 : BREATH/SOUND =================
  else if (mode == 2) {
    breathModeLoop();
    breathEmergencyManager();
  }

  // ========== SEND DATA PERIODICALLY ==========
  if (millis() - lastSend >= sendInterval) {
    sendToServer();
    lastSend = millis();
  }

  // ========== POLL SERVER COMMANDS ==========
  if (millis() - lastPoll >= pollInterval) {
    checkServerCommands();
    lastPoll = millis();
  }

  delay(10);
}

//////////////////////////////////////////////////////
//               BREATH MODE FUNCTIONS
//////////////////////////////////////////////////////

void breathModeLoop() {
  int breathLevel = smoothBreath();
  unsigned long now = millis();

  if (now - lastSerialPrint > 500) {
    Serial.print("[BREATH] Level: ");
    Serial.print(breathLevel);
    Serial.print("  Count: ");
    Serial.print(breathCount);
    Serial.print("  Fan: ");
    Serial.print(relay1State ? "ON" : "OFF");
    Serial.print("  Light: ");
    Serial.println(relay2State ? "ON" : "OFF");
    lastSerialPrint = now;
  }

  // Don't detect new breaths if emergency is active
  if (breathEmergencyActive || breathWaitingCall) return;

  if (!breathing && breathLevel > startThreshold) {
    breathing = true;
    breathCount++;
    lastBreathTime = now;
    Serial.print("[BREATH] Detected! Count = ");
    Serial.println(breathCount);
  }

  if (breathing && breathLevel < stopThreshold)
    breathing = false;

  if (breathCount > 0 && now - lastBreathTime > 3000) {
    if (breathCount == 1) {
      toggleFan();
      Serial.print("[BREATH] 1 breath → Fan toggled → Now: ");
      Serial.println(relay1State ? "ON" : "OFF");
    }
    else if (breathCount == 2) {
      toggleLight();
      Serial.print("[BREATH] 2 breaths → Light toggled → Now: ");
      Serial.println(relay2State ? "ON" : "OFF");
    }
    else if (breathCount >= 3) {
      // Emergency! Activate Relay3 for 5 seconds then call MacroDroid
      Serial.println("[BREATH] 3+ breaths → EMERGENCY! Relay3 ON for 5s then MacroDroid call.");
      Serial.println("[BREATH] Press BUTTON_CANCEL (Pin 5) within 5s to cancel!");
      digitalWrite(RELAY3, LOW);
      breathEmergencyActive = true;
      breathWaitingCall = true;
      breathCancelled = false;
      breathRelay3Start = millis();
    }
    breathCount = 0;
  }
}

// =================================================
//    BREATH EMERGENCY MANAGER
//    Called every loop when mode == 2
// =================================================
void breathEmergencyManager() {
  if (!breathEmergencyActive) return;

  unsigned long elapsed = millis() - breathRelay3Start;

  // Check for cancel button press (Pin 5)
  if (digitalRead(BUTTON_CANCEL) == LOW && !breathCancelled) {
    breathCancelled = true;
    breathEmergencyActive = false;
    breathWaitingCall = false;
    digitalWrite(RELAY3, HIGH);
    Serial.println("[BREATH] Emergency CANCELLED by button press!");
    sendToServer();
    lastSend = millis();
    return;
  }

  // After 5 seconds: turn off relay and trigger MacroDroid call
  if (elapsed >= relay3Duration) {
    digitalWrite(RELAY3, HIGH);
    breathEmergencyActive = false;
    breathWaitingCall = false;

    if (!breathCancelled) {
      Serial.println("[BREATH] 5s elapsed → Relay3 OFF → Triggering MacroDroid call!");
      triggerMacroDroid();
    }

    sendToServer();
    lastSend = millis();
  }
  else {
    // Still waiting — print countdown every second
    static unsigned long lastCountdown = 0;
    if (millis() - lastCountdown >= 1000) {
      int remaining = (int)((relay3Duration - elapsed) / 1000) + 1;
      Serial.print("[BREATH] Emergency active... Call in ");
      Serial.print(remaining);
      Serial.println("s (press Pin 5 to cancel)");
      lastCountdown = millis();
    }
  }
}

// =================================================
// AUTO CALIBRATION
// =================================================
void autoCalibrate() {
  Serial.println("Auto Calibrating... Keep Silent");
  long total = 0;
  for (int i = 0; i < 100; i++) {
    total += readBreathLevel();
    delay(40);
  }
  int noise = total / 100;
  startThreshold = noise + 80;
  stopThreshold  = noise + 30;
  Serial.print("Calibration Done. Noise floor: ");
  Serial.print(noise);
  Serial.print("  Start threshold: ");
  Serial.print(startThreshold);
  Serial.print("  Stop threshold: ");
  Serial.println(stopThreshold);
}

// =================================================
int smoothBreath() {
  long total = 0;
  for (int i = 0; i < 5; i++) {
    total += readBreathLevel();
    delay(5);
  }
  return total / 5;
}

int readBreathLevel() {
  unsigned long startMillis = millis();
  int signalMax = 0;
  int signalMin = 4095;
  while (millis() - startMillis < SAMPLE_WINDOW) {
    int sample = analogRead(MIC_PIN);
    if (sample > signalMax) signalMax = sample;
    if (sample < signalMin) signalMin = sample;
  }
  return signalMax - signalMin;
}

//////////////////////////////////////////////////////
// RELAY HELPERS
//////////////////////////////////////////////////////

void toggleFan() {
  relay1State = !relay1State;
  digitalWrite(RELAY1, relay1State ? LOW : HIGH);
  Serial.print("[RELAY] Fan → ");
  Serial.println(relay1State ? "ON" : "OFF");
}

void toggleLight() {
  relay2State = !relay2State;
  digitalWrite(RELAY2, relay2State ? LOW : HIGH);
  Serial.print("[RELAY] Light → ");
  Serial.println(relay2State ? "ON" : "OFF");
}

// =================================================
//    RELAY3 MANAGER (IR mode)
// =================================================
void relay3Manager() {
  // Auto-off after 5s
  if (relay3Active && millis() - relay3Start >= relay3Duration) {
    digitalWrite(RELAY3, HIGH);
    relay3Active = false;
    Serial.println("[IR] Relay3 auto-OFF after 5s");
  }

  // Cancel window logic
  if (waitingForCancel) {
    if (digitalRead(BUTTON_CANCEL) == LOW) {
      digitalWrite(RELAY3, HIGH);
      relay3Active = false;
      waitingForCancel = false;
      Serial.println("[IR] Emergency CANCELLED by button press!");
    }
    else if (millis() - cancelStart >= cancelWindow) {
      Serial.println("[IR] Cancel window expired → Triggering MacroDroid call!");
      triggerMacroDroid();
      waitingForCancel = false;
    }
    else {
      // Countdown print
      static unsigned long lastIRCountdown = 0;
      if (millis() - lastIRCountdown >= 1000) {
        int remaining = (int)((cancelWindow - (millis() - cancelStart)) / 1000);
        Serial.print("[IR] Cancel remaining: ");
        Serial.print(remaining);
        Serial.println("s");
        lastIRCountdown = millis();
      }
      
    }
  }
}
