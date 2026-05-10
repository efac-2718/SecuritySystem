#include <esp_now.h>
#include <WiFi.h>

// Destination: Receiver Hub MAC
uint8_t receiverAddress[] = {0x68, 0xFE, 0x71, 0x0B, 0xB5, 0xD8}; 

const int emergencyButton = 14; // Updated to D14
const int statusLed = 2;        

unsigned long lastHeartbeat = 0;
bool isEmergency = false;

void setup() {
  Serial.begin(115200);
  pinMode(emergencyButton, INPUT_PULLUP);
  pinMode(statusLed, OUTPUT);

  WiFi.mode(WIFI_STA);
  esp_now_init();

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  esp_now_add_peer(&peerInfo);
}

void loop() {
  // Trigger Emergency Alert
  if (digitalRead(emergencyButton) == LOW && !isEmergency) {
    delay(50); // Debounce
    const char* msg = "START_101";
    esp_now_send(receiverAddress, (uint8_t *) msg, strlen(msg));
    isEmergency = true;
    digitalWrite(statusLed, HIGH);
  }

  // Heartbeat to keep Room 101 "Online" (Green) on display
  if (millis() - lastHeartbeat > 5000) {
    const char* hb = "HB_101";
    esp_now_send(receiverAddress, (uint8_t *) hb, strlen(hb));
    lastHeartbeat = millis();
  }
}
