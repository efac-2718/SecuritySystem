#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "DHT.h"

// --- Pin Mapping ---
#define DHTPIN1     4   
#define DHTPIN2     5   
#define BUTTON_PIN 14  
#define BUZZER_PIN 27  
#define MQ2_1_PIN  32  
#define MQ2_2_PIN  33  
#define DHTTYPE DHT11

// --- Thresholds ---
const float FIRE_TEMP_THRESHOLD = 50.0;
const int GAS_ALARM_THRESHOLD = 5000; 

// --- ESP-NOW Configuration ---
uint8_t receiverMAC[] = {0x68, 0xFE, 0x71, 0x0B, 0xB5, 0xD8}; // Receiver MAC

typedef struct struct_message {
    bool emergency;
    char reason[32]; 
    float temp1;
    float hum1;
    float temp2;
    float hum2;
    int gas1;
    int gas2;
} struct_message;

struct_message myData;

DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

// Calibration & Smoothing
int offset1 = 800; 
int offset2 = 0; 
float filterFactor = 0.1; 
float smoothedGas1 = 0;
float smoothedGas2 = 0;

bool emergencyActive = false;
bool lastButtonState = HIGH;      // Track the previous physical state of the button

unsigned long lastReadTime = 0;
const int interval = 2000; 

unsigned long lastPingTime = 0;
const unsigned long pingInterval = 2000; // Send updates every 2 seconds

unsigned long lastBuzzerToggle = 0; // Timer for non-blocking buzzer
bool buzzerState = LOW;

// Helper function to send the data structure to the Receiver
void sendData() {
  esp_err_t result = esp_now_send(receiverMAC, (uint8_t *) &myData, sizeof(myData));
  if (result == ESP_OK) {
    Serial.println("Data sent successfully");
  } else {
    Serial.println("Error sending data");
  }
}

int32_t getHotspotChannel(const char *target_ssid) {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == target_ssid) {
      return WiFi.channel(i);
    }
  }
  return 1; // Fallback to channel 1 if hotspot isn't active
}

void setup() {
  Serial.begin(115200);
  
  dht1.begin();
  dht2.begin();
  pinMode(MQ2_1_PIN, INPUT);
  pinMode(MQ2_2_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); 
  pinMode(BUZZER_PIN, OUTPUT);

  // 1. Start WiFi in Station Mode to scan
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // 2. Dynamically find the hotspot's channel
  Serial.println("Scanning for Hotspot channel...");
  int32_t dynamicChannel = getHotspotChannel("Dialog 4G 136");
  Serial.print("Hotspot found! Aligning system to Channel: "); 
  Serial.println(dynamicChannel);

  // 3. Force the ESP32 radio to use that channel
  esp_wifi_set_channel(dynamicChannel, WIFI_SECOND_CHAN_NONE); 
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // 4. Register peer with the matching channel
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = dynamicChannel; // Matched dynamically!
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  strcpy(myData.reason, "SYSTEM ONLINE");
  myData.emergency = false;

  Serial.println("--- System Online: Monitoring & Wireless Started ---");
}

void loop() {
  // 1. LATCH SWITCH MONITORING
  bool currentButtonState = digitalRead(BUTTON_PIN);
  
  // Check if the physical switch changed positions
  if (currentButtonState != lastButtonState) {
    
    // LOW means the latch switch is pressed down/closed.
    if (currentButtonState == LOW) { 
      emergencyActive = false; // Latched position = Normal / Cleared
      strcpy(myData.reason, "CLEARED");
      Serial.println("\n[OK] LATCH SWITCH -> EMERGENCY CLEARED");
    } else { 
      emergencyActive = true;  // Released/Popped up position = Alarm Active
      strcpy(myData.reason, "MANUAL TRIGGER");
      Serial.println("\n[!] LATCH SWITCH -> EMERGENCY ACTIVE");
    }
    
    // Immediate transmission on manual switch change
    myData.emergency = emergencyActive;
    sendData();
    delay(50); // Debounce
  }
  lastButtonState = currentButtonState;

  // 2. SENSOR MONITORING, AUTOMATIC ALARMS & TIME-BASED TRANSMISSION
  if (millis() - lastReadTime >= interval) {
    lastReadTime = millis();

    float t1 = dht1.readTemperature();
    float h1 = dht1.readHumidity();
    float t2 = dht2.readTemperature();
    float h2 = dht2.readHumidity();

    int raw1 = analogRead(MQ2_1_PIN) + offset1;
    int raw2 = analogRead(MQ2_2_PIN) + offset2;
    smoothedGas1 = (raw1 * filterFactor) + (smoothedGas1 * (1.0 - filterFactor));
    smoothedGas2 = (raw2 * filterFactor) + (smoothedGas2 * (1.0 - filterFactor));

    // Handle automated state changes ONLY if manual override isn't forcing emergencyActive to true
    if (!emergencyActive) {
      if (t1 > FIRE_TEMP_THRESHOLD || t2 > FIRE_TEMP_THRESHOLD) {
        emergencyActive = true;
        strcpy(myData.reason, "FIRE HAZARD");
        Serial.println("\n[!] AUTO-ALARM: Fire Detected!");
      } 
      else if ((int)smoothedGas1 > GAS_ALARM_THRESHOLD || (int)smoothedGas2 > GAS_ALARM_THRESHOLD) {
        emergencyActive = true;
        strcpy(myData.reason, "GAS LEAK");
        Serial.println("\n[!] AUTO-ALARM: Gas Leak Detected!");
      }
    }

    // Populate the global structure
    myData.emergency = emergencyActive;
    myData.temp1 = t1;
    myData.hum1 = h1;
    myData.temp2 = t2;
    myData.hum2 = h2;
    myData.gas1 = (int)smoothedGas1;
    myData.gas2 = (int)smoothedGas2;

    // Send full data packet
    sendData();

    // Serial print block
    Serial.println("------------------------------------");
    Serial.print("DHT1: "); Serial.print(t1); Serial.print("C / "); Serial.print(h1); Serial.println("%");
    Serial.print("DHT2: "); Serial.print(t2); Serial.print("C / "); Serial.print(h2); Serial.println("%");
    Serial.print("Gas levels: MQ2-1: "); Serial.print(myData.gas1); 
    Serial.print(" | MQ2-2: "); Serial.println(myData.gas2);
    Serial.print("Status: "); Serial.println(emergencyActive ? "ALARM ACTIVE" : "NORMAL");
  }

  // 3. LOCAL BUZZER CONTROL
  if (emergencyActive) {
    if (millis() - lastBuzzerToggle >= 150) {
      buzzerState = !buzzerState; 
      digitalWrite(BUZZER_PIN, buzzerState);
      lastBuzzerToggle = millis();
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}