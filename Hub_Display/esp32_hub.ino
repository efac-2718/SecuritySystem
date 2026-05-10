#include <WiFi.h>
#include <esp_now.h>

// --- MAC ADDRESSES ---
// !!! IMPORTANT: Replace with your actual Sender MAC addresses !!!
uint8_t SENDER1_MAC[] = {0xEC, 0xE3, 0x34, 0x9A, 0x0F, 0x94};
uint8_t SENDER2_MAC[] = {0x6C, 0xC8, 0x40, 0x8C, 0x5A, 0x68};

// --- PIN DEFINITIONS ---
const int ledPin = 2;        // Built-in LED on ESP32 (Visual Alarm)
const int buttonPin = 4;     // Physical Acknowledge Button on Receiver
const int buzzerPin = 16;    // Dedicated Buzzer Pin (Example GPIO 16)

// --- SERIAL COMMUNICATION TO NANO ---
#define SERIAL_TO_NANO Serial2
#define SERIAL_BAUD 9600
const int NANO_TX_PIN = 17; // ESP32 TX2 pin (Connect to Nano RX)
const int NANO_RX_PIN = 16; // ESP32 RX2 pin (Unused)

// --- TIMEOUT CONSTANTS ---
const unsigned long POWER_OFF_TIMEOUT = 10000;      // 10 seconds without heartbeat is offline
const unsigned long ACK_REMINDER_TIMEOUT = 20000;   // 20 seconds for escalation/reminder

// --- ALARM STATUS TRACKING ---
struct BedStatus {
  uint8_t mac[6];
  const char* id;
  bool isActive;
  bool isAcknowledged;
  unsigned long lastHeartbeatTime; 
  unsigned long acknowledgementTime; // Time the ACK button was pressed
  bool isPoweredOn;               
};

BedStatus beds[] = {
  {
    .mac = {0xEC, 0xE3, 0x34, 0x9A, 0x0F, 0x94},
    .id = "B01",
    .isActive = false,
    .isAcknowledged = false,
    .lastHeartbeatTime = 0,
    .acknowledgementTime = 0,
    .isPoweredOn = false
  },
  {
    .mac = {0x6C, 0xC8, 0x40, 0x8C, 0x5A, 0x68},
    .id = "B02",
    .isActive = false,
    .isAcknowledged = false,
    .lastHeartbeatTime = 0,
    .acknowledgementTime = 0,
    .isPoweredOn = false
  }
};
const int NUM_BEDS = 2;

// --- DEBOUNCE & BLINKING ---
bool lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

bool isAnyAlarmActive = false;
bool isAnyAlarmUnacknowledged = false;
unsigned long lastBlinkToggle = 0;
unsigned long blinkInterval = 500;
bool ledState = LOW;

// --- UTILITY FUNCTIONS ---

BedStatus* findBedByMac(const uint8_t *mac) {
  for (int i = 0; i < NUM_BEDS; i++) {
    if (memcmp(beds[i].mac, mac, 6) == 0) {
      return &beds[i];
    }
  }
  return nullptr;
}

void checkPowerStatus() {
  for (int i = 0; i < NUM_BEDS; i++) {
    if (millis() - beds[i].lastHeartbeatTime > POWER_OFF_TIMEOUT) {
      beds[i].isPoweredOn = false;
    } else {
      beds[i].isPoweredOn = true;
    }
  }
}

// Function to generate the complete status string for the Nano
void sendStatusToNano() {
  String statusMsg = "STATUS:";
  isAnyAlarmUnacknowledged = false;
  isAnyAlarmActive = false;

  checkPowerStatus(); 

  for (int i = 0; i < NUM_BEDS; i++) {
    
    // Check 1: POWER STATUS (Highest Priority for display)
    if (!beds[i].isPoweredOn) {
      statusMsg += String(beds[i].id) + "_OFF"; // OFFLINE (GRAY)
    }
    // Check 2: ALARM STATUS (If powered on)
    else if (beds[i].isActive) { 
      isAnyAlarmActive = true;
      if (!beds[i].isAcknowledged) {
        isAnyAlarmUnacknowledged = true;
        statusMsg += String(beds[i].id) + "_CALL"; // CALL (RED)
      } else {
        statusMsg += String(beds[i].id) + "_ACK"; // ACK (YELLOW)
      }
    } 
    // Check 3: READY STATUS (If powered on, no alarm)
    else {
      statusMsg += String(beds[i].id) + "_CLEAR"; // READY (GREEN)
    }
    
    if (i < NUM_BEDS - 1) {
      statusMsg += "|";
    }
  }
  
  SERIAL_TO_NANO.println(statusMsg);
  Serial.println("Nano Status: " + statusMsg);
}

void sendMessage(uint8_t *targetMac, const char *msg) {
  esp_now_send(targetMac, (uint8_t *)msg, strlen(msg));
}

void handleBlinkingAndBuzzer() {
  if (isAnyAlarmUnacknowledged) {
    if (millis() - lastBlinkToggle >= blinkInterval) {
      lastBlinkToggle = millis();
      ledState = !ledState;
      digitalWrite(ledPin, ledState);
      digitalWrite(buzzerPin, ledState); // Toggle buzzer
    }
  } else {
    digitalWrite(ledPin, LOW);
    digitalWrite(buzzerPin, LOW);
  }
}

// --- REMINDER ESCALATION HANDLER ---
void handleReminder() {
  for (int i = 0; i < NUM_BEDS; i++) {
    // Check if the alarm is ACTIVE, ACKNOWLEDGED, and the timeout has passed
    if (beds[i].isActive && 
        beds[i].isAcknowledged && 
        beds[i].acknowledgementTime != 0 &&
        millis() - beds[i].acknowledgementTime > ACK_REMINDER_TIMEOUT) 
    {
      Serial.println("ESCALATION: Alarm for " + String(beds[i].id) + " timed out. Reactivating CALL status.");
      
      // 1. Reactivate the CALL status locally (clearing ACK flag)
      beds[i].isAcknowledged = false;
      
      // 2. Clear the timer so it doesn't immediately re-trigger
      beds[i].acknowledgementTime = 0; 
      
      // 3. Send a new REMIND message to the Sender (using the .c_str() fix)
      String remindMsg = "REMIND_" + String(beds[i].id); 
      sendMessage(beds[i].mac, remindMsg.c_str());
      
      // 4. Update the display and start the buzzer/LED
      sendStatusToNano();
    }
  }
}

// --- RECEIVED DATA HANDLER ---
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  String msg = "";
  for (int i = 0; i < len; i++) msg += (char)data[i];
  
  BedStatus* bed = findBedByMac(info->src_addr);
  if (!bed) {
    Serial.println("Unknown Sender.");
    return;
  }
  
  // Update Heartbeat Time for ANY message at the start
  bed->lastHeartbeatTime = millis();
  
  // Check if it's just a Heartbeat message
  if (msg.startsWith("HB_")) { 
    Serial.println("Heartbeat from " + String(bed->id));
    sendStatusToNano(); 
    return;
  }
  
  // --- START / REMIND MESSAGE ---
  if (msg.startsWith("START_") || msg.startsWith("REMIND_")) {
    bed->isActive = true;
    bed->isAcknowledged = false;
    bed->acknowledgementTime = 0; // Clear reminder timer
    Serial.println("Alarm START/REMIND received.");
  }
  
  // --- STOP MESSAGE (PHYSICAL RESET) ---
  else if (msg.startsWith("STOP")) {
    bed->isActive = false;
    bed->isAcknowledged = false;
    bed->acknowledgementTime = 0; // Clear reminder timer
    Serial.println("Alarm STOP received (Physical Reset).");
  }
  
  sendStatusToNano();
}

void handleButton() {
  bool reading = digitalRead(buttonPin);
  if (reading != lastButtonReading) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static bool lastStable = HIGH;
    if (lastStable == LOW && reading == HIGH) { // Button released (Trigger Action: ACKNOWLEDGE)
      
      for (int i = 0; i < NUM_BEDS; i++) {
        if (beds[i].isActive && !beds[i].isAcknowledged) {
          // 1. Acknowledge locally
          beds[i].isAcknowledged = true;
          // 2. START THE REMINDER TIMER (CRITICAL ADDITION)
          beds[i].acknowledgementTime = millis(); 
          
          // 3. Send ACK back to the Sender
          sendMessage(beds[i].mac, "ACK");
          Serial.println("ACK sent to " + String(beds[i].id) + ". Reminder timer started (20s).");
          
          sendStatusToNano(); 
          break; 
        }
      }
    }
    lastStable = reading;
  }
  lastButtonReading = reading;
}

// --- SETUP ---
unsigned long lastStatusUpdate = 0;
void setup() {
  Serial.begin(115200);
  
  SERIAL_TO_NANO.begin(SERIAL_BAUD, SERIAL_8N1, NANO_RX_PIN, NANO_TX_PIN);

  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
      Serial.println("ESP-NOW init failed!");
      return;
  }
  
  for (int i = 0; i < NUM_BEDS; i++) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, beds[i].mac, 6);
    esp_now_add_peer(&peer);
  }

  esp_now_register_recv_cb(onReceive);
  Serial.println("Receiver ready. Tracking " + String(NUM_BEDS) + " beds.");
  sendStatusToNano();
}

void loop() {
  handleBlinkingAndBuzzer();
  handleButton();
  handleReminder(); // CHECK FOR TIMEOUTS
  
  if (millis() - lastStatusUpdate > 1000) { // Check and send status every second
    sendStatusToNano();
    lastStatusUpdate = millis();
  }
}
