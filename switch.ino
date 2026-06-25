#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "DHT.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Pin Mapping ---
#define DHTPIN1     4
#define DHTPIN2     5
#define BUTTON_PIN 14
#define BUZZER_PIN 27
#define MQ2_1_PIN  32
#define MQ2_2_PIN  33
#define MAX9814_PIN 34
#define DHTTYPE DHT11

// --- Thresholds ---
const float FIRE_TEMP_THRESHOLD = 50.0;
const int   GAS_ALARM_THRESHOLD = 5000;

// --- ESP-NOW ---
uint8_t receiverMAC[] = {0x68, 0xFE, 0x71, 0x0B, 0xB5, 0xD8};

typedef struct struct_message {
    bool  emergency;
    char  reason[32];
    float temp1, hum1, temp2, hum2;
    int   gas1, gas2;
} struct_message;

struct_message myData;

DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

// --- Gas smoothing ---
int   offset1 = 800, offset2 = 0;
float filterFactor = 0.1;
float smoothedGas1 = 0, smoothedGas2 = 0;

// --- MAX9814 ---
int noiseLevel = 0;

// --- MQTT ---
const char* mqtt_server    = "192.168.8.139";
const int   mqtt_port      = 1883;
const char* mqtt_pub_topic = "esp32/sensor";

WiFiClient    espClient;
PubSubClient  client(espClient);

// --- State ---
bool emergencyActive   = false;
bool lastButtonState   = HIGH;
bool wifiConnecting    = false;

unsigned long lastReadTime             = 0;
unsigned long lastBuzzerToggle         = 0;
unsigned long lastMQTTReconnectAttempt = 0;
unsigned long lastMQTTPublish          = 0;

const unsigned long SENSOR_INTERVAL   = 2000;
const unsigned long MQTT_PUB_INTERVAL = 2500;
bool pendingPublish = false;

bool buzzerState = LOW;

// -------------------------------------------------------

void sendData() {
    esp_err_t result = esp_now_send(receiverMAC, (uint8_t*)&myData, sizeof(myData));
    Serial.println(result == ESP_OK ? "ESP-NOW sent" : "ESP-NOW error");
}

void publishSensorData() {
    if (!client.connected()) {
        Serial.println("MQTT publish skipped (not connected)");
        return;
    }

    StaticJsonDocument<200> doc;
    doc["dht11_1"] = myData.temp1;
    doc["dht11_2"] = myData.temp2;
    doc["MQ2_1"]   = myData.gas1;
    doc["MQ2_2"]   = myData.gas2;
    doc["noise"]   = noiseLevel;

    char buffer[200];
    serializeJson(doc, buffer);

    if (client.publish(mqtt_pub_topic, buffer)) {
        Serial.print("MQTT Published: ");
        Serial.println(buffer);
    } else {
        Serial.println("MQTT Publish failed — will retry next interval");
        pendingPublish = true;
    }
}

void handleMQTTReconnect() {
    unsigned long now = millis();
    if (now - lastMQTTReconnectAttempt < 5000) return;
    lastMQTTReconnectAttempt = now;

    Serial.println("MQTT: attempting reconnect...");
    espClient.setTimeout(3000);
    if (client.connect("esp32-sensor-node")) {
        Serial.println("MQTT: reconnected");
        lastMQTTReconnectAttempt = 0;
        if (pendingPublish) {
            publishSensorData();
            pendingPublish = false;
        }
    } else {
        Serial.print("MQTT: connect failed, rc=");
        Serial.println(client.state());
    }
}

int32_t getHotspotChannel(const char* target_ssid) {
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == target_ssid) return WiFi.channel(i);
    }
    return 1;
}

const char* ssid     = "Dialog 4G 136";
const char* password = "84A6EFC1";

void startWiFi() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, password);
    WiFi.setAutoReconnect(true);
    wifiConnecting = true;
    Serial.println("WiFi connection started...");
}

// -------------------------------------------------------

void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    Serial.println("Scanning for Hotspot channel...");
    int32_t dynamicChannel = getHotspotChannel(ssid);
    Serial.print("Channel: "); Serial.println(dynamicChannel);

    startWiFi();

    espClient.setTimeout(3000);
    client.setServer(mqtt_server, mqtt_port);
    client.setKeepAlive(60);

    dht1.begin(); dht2.begin();
    pinMode(MQ2_1_PIN,   INPUT);
    pinMode(MQ2_2_PIN,   INPUT);
    pinMode(MAX9814_PIN, INPUT);
    pinMode(BUTTON_PIN,  INPUT_PULLUP);
    pinMode(BUZZER_PIN,  OUTPUT);

    esp_wifi_set_channel(dynamicChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMAC, 6);
    peerInfo.channel = dynamicChannel;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    strcpy(myData.reason, "SYSTEM ONLINE");
    myData.emergency = false;

    Serial.println("--- System Online ---");
}

// -------------------------------------------------------

void loop() {

    // ── 1. BUZZER ────────────────────────────────────────────────────────
    if (emergencyActive) {
        if (millis() - lastBuzzerToggle >= 150) {
            buzzerState = !buzzerState;
            digitalWrite(BUZZER_PIN, buzzerState);
            lastBuzzerToggle = millis();
        }
    } else {
        digitalWrite(BUZZER_PIN, LOW);
    }

    // ── 2. LATCH BUTTON ──────────────────────────────────────────────────
    bool currentButtonState = digitalRead(BUTTON_PIN);
    if (currentButtonState != lastButtonState) {
        if (currentButtonState == LOW) {
            emergencyActive = false;
            strcpy(myData.reason, "CLEARED");
            Serial.println("[OK] LATCH -> CLEARED");
        } else {
            emergencyActive = true;
            strcpy(myData.reason, "MANUAL TRIGGER");
            Serial.println("[!] LATCH -> EMERGENCY");
        }
        myData.emergency = emergencyActive;
        sendData();
        delay(50);
    }
    lastButtonState = currentButtonState;

    // ── 3. SENSOR READ (every 2 s) ───────────────────────────────────────
    if (millis() - lastReadTime >= SENSOR_INTERVAL) {
        lastReadTime = millis();

        float t1 = dht1.readTemperature(), h1 = dht1.readHumidity();
        float t2 = dht2.readTemperature(), h2 = dht2.readHumidity();

        int raw1 = analogRead(MQ2_1_PIN) + offset1;
        int raw2 = analogRead(MQ2_2_PIN) + offset2;
        smoothedGas1 = raw1 * filterFactor + smoothedGas1 * (1.0f - filterFactor);
        smoothedGas2 = raw2 * filterFactor + smoothedGas2 * (1.0f - filterFactor);

        noiseLevel = analogRead(MAX9814_PIN);

        if (!emergencyActive) {
            if (t1 > FIRE_TEMP_THRESHOLD || t2 > FIRE_TEMP_THRESHOLD) {
                emergencyActive = true;
                strcpy(myData.reason, "FIRE HAZARD");
                Serial.println("[!] AUTO: Fire!");
            } else if ((int)smoothedGas1 > GAS_ALARM_THRESHOLD || (int)smoothedGas2 > GAS_ALARM_THRESHOLD) {
                emergencyActive = true;
                strcpy(myData.reason, "GAS LEAK");
                Serial.println("[!] AUTO: Gas Leak!");
            }
        }

        myData.emergency = emergencyActive;
        myData.temp1 = t1; myData.hum1 = h1;
        myData.temp2 = t2; myData.hum2 = h2;
        myData.gas1  = (int)smoothedGas1;
        myData.gas2  = (int)smoothedGas2;

        sendData();
        pendingPublish = true;

        Serial.printf("DHT1: %.1fC/%.1f%%  DHT2: %.1fC/%.1f%%  Gas: %d | %d  Noise: %d  [%s]\n",
            t1, h1, t2, h2, myData.gas1, myData.gas2, noiseLevel,
            emergencyActive ? "ALARM" : "NORMAL");
    }

    // ── 4. WiFi status ───────────────────────────────────────────────────
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiConnecting) {
            Serial.print("WiFi OK, IP: "); Serial.println(WiFi.localIP());
            wifiConnecting = false;
        }

        // ── 5. MQTT ──────────────────────────────────────────────────────
        if (client.connected()) {
            client.loop();

            if (pendingPublish && millis() - lastMQTTPublish >= MQTT_PUB_INTERVAL) {
                lastMQTTPublish = millis();
                publishSensorData();
                pendingPublish = false;
            }
        } else {
            handleMQTTReconnect();
        }

    } else {
        if (!wifiConnecting) {
            Serial.println("WiFi dropped, reconnecting...");
            wifiConnecting = true;
        }
    }
}
