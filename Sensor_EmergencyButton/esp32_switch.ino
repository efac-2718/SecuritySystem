#include <esp_now.h>
#include <WiFi.h>
#include "DHT.h"

// --- Pin Mapping ---
#define DHTPIN1    4   
#define DHTPIN2    5   
#define BUTTON_PIN 14  
#define BUZZER_PIN 27  
#define MQ2_1_PIN  32  
#define MQ2_2_PIN  33  
#define DHTTYPE DHT11

// --- Thresholds ---
const float FIRE_TEMP_THRESHOLD = 50.0;
const int GAS_ALARM_THRESHOLD = 2200; 

// --- ESP-NOW Configuration ---
uint8_t broadcastAddress[] = {0x68, 0xFE, 0x71, 0x0B, 0xB5, 0xD8};

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
esp_now_peer_info_t peerInfo;

DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

// Calibration & Smoothing
int offset1 = 800; 
int offset2 = 0; 
float filterFactor = 0.1; 
float smoothedGas1 = 0;
float smoothedGas2 = 0;

bool emergencyActive = false;
unsigned long lastReadTime = 0;
const int interval = 2000; 

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // Prints status to verify communication is working
  Serial.print("\r\nWireless Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

void setup() {
  Serial.begin(115200);
  
  dht1.begin();
  dht2.begin();
  pinMode(MQ2_1_PIN, INPUT);
  pinMode(MQ2_2_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT); 
  pinMode(BUZZER_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(esp_now_send_cb_t(OnDataSent));
  
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
  }
  
  Serial.println("--- System Online: Monitoring Started ---");
}

void loop() {
  // 1. MANUAL BUTTON TRIGGER
  if (digitalRead(BUTTON_PIN) == LOW) {
    emergencyActive = !emergencyActive;
    
    myData.emergency = emergencyActive;
    if (emergencyActive) {
      strcpy(myData.reason, "MANUAL TRIGGER");
      Serial.println("\n[!] EMERGENCY: Manual Button Pressed");
    } else {
      strcpy(myData.reason, "CLEARED");
      Serial.println("\n[OK] EMERGENCY: Cleared");
    }

    esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
    delay(500); 
  }

  // 2. BUZZER CONTROL
  if (emergencyActive) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // 3. SENSOR MONITORING (This prints values to the Serial Monitor)
  if (millis() - lastReadTime >= interval) {
    lastReadTime = millis();

    // Read Sensors
    float t1 = dht1.readTemperature();
    float h1 = dht1.readHumidity();
    float t2 = dht2.readTemperature();
    float h2 = dht2.readHumidity();

    int raw1 = analogRead(MQ2_1_PIN) + offset1;
    int raw2 = analogRead(MQ2_2_PIN) + offset2;
    smoothedGas1 = (raw1 * filterFactor) + (smoothedGas1 * (1.0 - filterFactor));
    smoothedGas2 = (raw2 * filterFactor) + (smoothedGas2 * (1.0 - filterFactor));

    // Auto-Trigger Logic
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

    // Prepare Data for Sync
    myData.emergency = emergencyActive;
    myData.temp1 = t1;
    myData.hum1 = h1;
    myData.temp2 = t2;
    myData.hum2 = h2;
    myData.gas1 = (int)smoothedGas1;
    myData.gas2 = (int)smoothedGas2;

    // --- DISPLAY SENSOR DATA IN SERIAL MONITOR ---
    Serial.println("------------------------------------");
    Serial.print("DHT1: "); Serial.print(t1); Serial.print("C / "); Serial.print(h1); Serial.println("%");
    Serial.print("DHT2: "); Serial.print(t2); Serial.print("C / "); Serial.print(h2); Serial.println("%");
    Serial.print("Gas levels: MQ2-1: "); Serial.print(myData.gas1); 
    Serial.print(" | MQ2-2: "); Serial.println(myData.gas2);
    Serial.print("Status: "); Serial.println(emergencyActive ? "ALARM ACTIVE" : "NORMAL");

    // Send everything over the air
    esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
  }
}
