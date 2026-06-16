#include <Arduino.h>
#include <Wire.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define GSM_RX_PIN    26
#define GSM_TX_PIN    27
#define GSM_BAUD_RATE 9600

#define PIN_BUZZER 32
#define PIN_LED     2

const int RXD_DISPLAY = 16;
const int TXD_DISPLAY = 17;
HardwareSerial displaySerial(1);
#define DISPLAY_SERIAL displaySerial

HardwareSerial sim900(2);
uint8_t ROOM101_MAC[] = {0x70, 0x4B, 0xCA, 0x49, 0x16, 0xD0};
uint8_t WEARABLE_MAC[] = {0x70, 0x4B, 0xCA, 0x49, 0x16, 0xD1};

String contact1 = "+94764175179";
String contact2 = "+94764175179";
String emergency = "+94764175179";
String confirmMessage = "Hi. This message confirms that you are taking action.";

unsigned long alarmStartTime = 0;
unsigned long previousBlinkMillis = 0;
int ledState = LOW;
bool alarmActive = false;
String receivedEmployeeID = "UNKNOWN";

typedef struct fall_message {
 	char  msg[32];
  	float g_force;
  	char  employee_id[16];
} fall_message;

typedef struct room_message {
  	bool  emergency;
  	char  reason[32];
  	float temp1;
  	float hum1;
  	float temp2;
  	float hum2;
  	int   gas1;
  	int   gas2;
} room_message;

typedef struct fall_heartbeat {
	bool emergency;
} fall_heartbeat;


fall_message incomingFallPacket;
room_message myData;
fall_heartbeat fallData;


volatile bool fallSignalReceived = false;
float receivedGForce = 0;
unsigned long lastSeen101 = 0;
bool is101Online = false;
const unsigned long timeoutLimit = 10000;
bool lastEmergencyState = false;

unsigned long lastSeenWearable = 0;
bool isWearableOnline = false;
const unsigned long wearableTimeout = 10 * 60 * 1000;
//---------------------------------------------
//MQTT
//---------------------------------------------
WiFiClient espClient;
PubSubClient client(espClient);

const char* mqtt_server = "192.168.8.139";
const int   mqtt_port = 1883;
const char* mqtt_pub_topic = "esp32/employee";
const char* mqtt_sub_topic = "esp32/wearable1";
const char* mqtt_emergency_topic = "esp32/emergency";
const char* mqtt_reset_topic = "esp32/reset";

long lastReconnectAttemptMQTT = 0;

boolean reconnect() {
  	if (client.connect("esp32-client-1")) {
    		client.publish("outTopic","hello world");
    		client.subscribe("inTopic");
				client.subscribe(mqtt_reset_topic);
				Serial.println("MQTT Connected!");
				return true;
  	} else {
				Serial.println("MQTT Disconnected. Reconnecting...");
				return false;
		}
}


void callback(char* topic, byte* payload, unsigned int length) {
	String message;
	for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

	StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) { Serial.println("JSON parse error"); return; }

	if (strcmp(topic, mqtt_reset_topic) == 0){
		if (doc.containsKey("reset")){ 
			lastEmergencyState = false;
			myData.emergency = false;
			clearAlert();
		}
	} 
}

//-----------------------------------------------

//-----------------------------------------------
//WiFi
//-----------------------------------------------

const char* ssid = "Dialog 4G 136";
const char* password = "84A6EFC1";

unsigned long lastAttemptTimeWiFi = 0;
bool wifiConnecting = false;

void startWiFi() {
	WiFi.mode(WIFI_AP_STA);
    	WiFi.begin(ssid, password);
	WiFi.setAutoReconnect(true);
    	wifiConnecting = true;
    	lastAttemptTimeWiFi = millis();

    	Serial.println("WiFi connection started...");
}
//-----------------------------------------------

//-----------------------------------------------
//Alert
//-----------------------------------------------
void updateDisplay(){
	DISPLAY_SERIAL.println("101:EMERGENCY");
	Serial.println("[DISPLAY] Sent: 101:EMERGENCY");
	
}

void sendMQTTMessage(){
	if(strcmp(myData.reason, "MANUAL TRIGGER") == 0){
		const char* payload = "{\"type\": \"Button Emergency\", \"location\": \"A1 - A101\"}";
		client.publish(mqtt_emergency_topic,payload);
	}
	else if(strcmp(myData.reason, "FIRE HAZARD") == 0){
		const char* payload = "{\"type\": \"Fire\", \"location\": \"A1 - A101\"}";
		client.publish(mqtt_emergency_topic,payload);
	}
	else if(strcmp(myData.reason, "GAS LEAK") == 0){
		const char* payload = "{\"type\": \"Gas leak\", \"location\": \"A1 - A101\"}";
		client.publish(mqtt_emergency_topic,payload);
	}
}

void alert(){
  // Only trigger actions when the state CHANGES to true
  if (myData.emergency && !lastEmergencyState) {
    lastEmergencyState = true; // Lock the state
    updateDisplay();
    sendMQTTMessage();
  } 
}
void clearAlert(){
	DISPLAY_SERIAL.println("101:RESET");
	Serial.println("[DISPLAY] Sent: 101:RESET");
}

//-----------------------------------------------
//-----------------------------------------------
//ESP-NOW
//-----------------------------------------------

void initESPNow() {
	if (esp_now_init() != ESP_OK) {
		Serial.println("Error initialising ESP-NOW");
		return;
	}
	esp_now_register_recv_cb(onDataRecv);
}

void alive(){
				DISPLAY_SERIAL.println("101:ONLINE");
				//Identify change of status.
				Serial.println("[DISPLAY] Sent: 101:ONLINE");
				Serial.println("Room 101 is ONLINE");
}

void dead() {
			DISPLAY_SERIAL.println("101:OFFLINE");
			Serial.println("[DISPLAY] Sent: 101:OFFLINE");
			Serial.println("[WARNING] Room 101 heartbeat lost - OFFLINE");
}

bool connectionLost = false;
unsigned long lastPacketTime = 0;


unsigned long lastFallAlarmTime = 0;
const unsigned long FALL_ALARM_COOLDOWN = 300000;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {

	//---------------Room-101 packet---------------------------
	if (memcmp(info->src_addr, ROOM101_MAC, 6) == 0) {
			lastPacketTime = millis();
		if (len == sizeof(myData)) {
			memcpy(&myData, incomingData, sizeof(myData));
			if (connectionLost) {
					connectionLost = false;
					alive();
			}
		} 
	}  // --------------- Wearable packet ---------------------------
	else if (memcmp(info->src_addr, WEARABLE_MAC, 6) == 0) {
        	lastSeenWearable = millis();

        // Check if it matches fall alarm packet
        	if (len == sizeof(incomingFallPacket)) {
						memcpy(&incomingFallPacket, incomingData, sizeof(incomingFallPacket));
						if(strcmp(incomingFallPacket.msg, "FALL_DETECTED") == 0) {
							unsigned long now = millis();
							if (now - lastFallAlarmTime >= FALL_ALARM_COOLDOWN) {

								fallSignalReceived = true;
								receivedGForce = incomingFallPacket.g_force;
								receivedEmployeeID = String(incomingFallPacket.employee_id);
								lastFallAlarmTime = now;
								Serial.println("Fall alarm accepted");
							}
							else {
								Serial.println("Fall alarm ignored (cooldown active)");
							}
						}
        	}	 
        // Check if it matches standard heartbeat packet
        	else if (len == sizeof(fallData)) {
            		memcpy(&fallData, incomingData, sizeof(fallData));
        	} else {
            		Serial.print("Warning: Wearable packet size unrecognized. Got ");
            		Serial.println(len);
        	}
    }
}

void listening(){
	unsigned long timeout = 5000;

	if ((millis() - lastPacketTime > timeout) && !connectionLost){
		connectionLost = true;
		dead();
	}
}

//----------------------------------------------

//----------------------------------------------
//GSM
//----------------------------------------------
void clearSerialBuffer(){
	while (sim900.available()) sim900.read();
}


bool checkResponse(String target_response, long timeout) {
  long startTime = millis();
  while (millis() - startTime < timeout) {
    if (sim900.available()) {
      String response = sim900.readStringUntil('\n');
      response.trim();
      Serial.println("-> " + response);
      if (response.indexOf(target_response) != -1) {
        return true; 
      }
      if (response.indexOf("ERROR") != -1) {
        Serial.println("Command returned ERROR.");
        return false;
      }
    }
  }
  return false;
}

void initialiseGSM(){
	clearSerialBuffer();
	delay(3000);
	sim900.println("AT+CFSTERM"); checkResponse("OK", 3000);
	sim900.println("AT+CFSINIT"); checkResponse("OK", 3000);
	sim900.println("AT+DDET=1");  checkResponse("OK", 2000);
	sim900.println("AT+CLCC=1");  checkResponse("OK", 1000);

}
void sendSMS(String number, String msg) {
  Serial.println("SMS -> " + number);
  sim900.println("AT+CMGF=1");
  delay(4000);
  sim900.println("AT+CMGS=\"" + number + "\"");
  while (sim900.available()) {
    String r = sim900.readString(); r.trim();
    if (r.indexOf(">") != -1) { delay(3000); sim900.print(msg); sim900.write(26); }
  }
}

int emergencyContact1() {
  int keyPressed = -1;
  int retry = 0;
  while (retry < 2) {
    bool callActive = false;
    sim900.println("ATD" + contact1 + ";");
    Serial.println("Calling contact 1");
    long ringTimeout = 50000;
    long startTimeForRing = millis();
    while (millis() - startTimeForRing < ringTimeout && !callActive) {
      sim900.println("AT+CLCC");
      delay(500);
      while (sim900.available()) {
        String response = sim900.readStringUntil('\n');
        response.trim();
        if (response.startsWith("+CLCC:") && response.charAt(11) == '0') {
          sim900.println("AT+CPAMR=\"T2.amr\",0"); 
          Serial.println("Playing audio...");
          callActive = true;
          break;
        }
        if (response.indexOf("NO CARRIER") != -1 || response.indexOf("BUSY") != -1) {
          sim900.println("ATH");
          Serial.println("User hanged up");
          return -1;
        }
      }
    }
    if (callActive) {
      int playbackTimeout = 90000;
      long startTimeForPlayback = millis();
      while (millis() - startTimeForPlayback < playbackTimeout) {
        while (sim900.available()) {
          String response = sim900.readStringUntil('\n');
          response.trim();
          if (response.startsWith("+DTMF:") && (String(response.charAt(6)).toInt()==1 || String(response.charAt(6)).toInt()==2 || String(response.charAt(6)).toInt()==3)) {
            keyPressed = String(response.charAt(6)).toInt();
            Serial.println("Received DTMF key: "+String(keyPressed));
            sim900.println("ATH");
            return keyPressed;
          }
        }
      }
      sim900.println("ATH");
    }
    if(!callActive){
      retry++;
      sim900.println("ATH");
      delay(4000);
    } 
  }
  return -1;
}

int emergencyContact2() {
  int keyPressed = -1;
  int retry = 0;
  while (retry < 2) {
    bool callActive = false;
    sim900.println("ATD" + contact2 + ";");
    long ringTimeout = 50000;
    long startTimeForRing = millis();
    while (millis() - startTimeForRing < ringTimeout && !callActive) {
      sim900.println("AT+CLCC");
      delay(500);
      while (sim900.available()) {
        String response = sim900.readStringUntil('\n');
        response.trim();
        if (response.startsWith("+CLCC:") && response.charAt(11) == '0') {
          sim900.println("AT+CPAMR=\"T3.amr\",0");
          callActive = true;
          break;
        }
        if (response.indexOf("NO CARRIER") != -1 || response.indexOf("BUSY") != -1) {
          sim900.println("ATH");
          return -1;
        }
      }
    }
    if (callActive) {
      int playbackTimeout = 90000;
      long startTimeForPlayback = millis();
      while (millis() - startTimeForPlayback < playbackTimeout) {
        while (sim900.available()) {
          String response = sim900.readStringUntil('\n');
          response.trim();
          if (response.startsWith("+DTMF:") && (String(response.charAt(6)).toInt()==1 || String(response.charAt(6)).toInt()==2)) {
            keyPressed = String(response.charAt(6)).toInt();
            sim900.println("ATH");
            return keyPressed;
          }
        }
      }
      sim900.println("ATH");
    }
    if(!callActive){
      retry++;
      sim900.println("ATH");
      delay(4000);
    } 
  }
  return -1;
}

void emergencyServices(){
  Serial.println("Calling emergency services");
  sim900.println("ATD"+emergency+";");
  
  while(true){
  sim900.println("AT+CLCC");
  delay(2000);
  while (sim900.available()) {
        String response = sim900.readStringUntil('\n');
        response.trim();
        if (response.startsWith("+CLCC:") && response.charAt(11) == '0') {
          sim900.println("AT+CPAMR=\"T4.amr\",0");
          delay(12000);
          sim900.println("ATH");
          return;
        }
    }
  }
}


void handleEmergencyCalls() {
  int k1 = emergencyContact1();
  if (k1 == 1){ delay(4000); sendSMS(contact1, confirmMessage); return; }
  	if (k1 == 2 || k1 == -1) { delay(4000); int k2 = emergencyContact2();
  		if (k2 == 1){ delay(4000); sendSMS(contact2, confirmMessage); return; }
    	if (k2 == 2 || k2 == -1){ delay(4000); emergencyServices(); return; }
  	}
  if (k1 == 3){ delay(4000); emergencyServices(); return; }
}

void wearableAlert(){
	const char* payload = "{\"employeeId\": \"EMP-002\", \"floor\": \"01\"}";
	client.publish(mqtt_pub_topic, payload);
}

void handleAlarmActions() {
  if (!alarmActive) return;

  // Auto-clear after 5 s (mirrors the real commented-out logic)
  if (millis() - alarmStartTime >= 5000) {
    Serial.println("[ALARM] 5 s expired — clearing (mock).");
    digitalWrite(PIN_LED, LOW);
    noTone(PIN_BUZZER);
		wearableAlert();
		handleEmergencyCalls();
    alarmActive = false;
    return;
  }
}

void triggerEmergencyProtocol() {
  Serial.println("\n!!! FALL DETECTED !!!");
  Serial.print("Employee: "); Serial.println(receivedEmployeeID);
  Serial.print("G-Force:  "); Serial.print(receivedGForce); Serial.println(" G");

  if (!alarmActive) {
    alarmActive    = true;
    alarmStartTime = millis();
    Serial.println("ALARM TRIGGERED — 5 s warning phase.");
  }
}

void checkFallStatus() {
  if (fallSignalReceived) {
    fallSignalReceived = false;
    triggerEmergencyProtocol();
  }
}


//----------------------------------------------
void setup() {
    	Serial.begin(115200);
    	startWiFi();
			initESPNow();

			DISPLAY_SERIAL.begin(9600, SERIAL_8N1, RXD_DISPLAY, TXD_DISPLAY);
			sim900.begin(GSM_BAUD_RATE, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

      client.setServer(mqtt_server, mqtt_port);
  		client.setCallback(callback);

			pinMode(PIN_LED,OUTPUT);
			initialiseGSM();
			
			lastReconnectAttemptMQTT = 0;
}

void loop() {
//-----------------------------------------------
//WiFi
//-----------------------------------------------
 	if (WiFi.status() == WL_CONNECTED) {
        	if (wifiConnecting) {
            		Serial.println("WiFi connected!");
            		Serial.print("IP: ");
            		Serial.println(WiFi.localIP());
            		wifiConnecting = false;
                Serial.println(WiFi.channel());
        	}
//-----------------------------------------------
//MQTT
//-----------------------------------------------

		if (!client.connected()) {
			unsigned long now = millis();
			if (now - lastReconnectAttemptMQTT > 5000) {
				lastReconnectAttemptMQTT = now;
				if (reconnect()) {
					lastReconnectAttemptMQTT = 0;
				}
			}
		} else {
			client.loop();
		}
//------------------------------------------------
    	}
 	else {
        	if (!wifiConnecting) {
            		Serial.println("WiFi dropped reconnecting in background...");
            		wifiConnecting = true;
        	}
    	}
//-----------------------------------------------

//-----------------------------------------------
//ESP-NOW Heartbeats
//-----------------------------------------------
listening();
//-----------------------------------------------

//-----------------------------------------------
//room-alerts
//-----------------------------------------------
alert();
//-----------------------------------------------

//-----------------------------------------------
//wearable-alerts
//-----------------------------------------------
checkFallStatus();
handleAlarmActions();
//------------------------------------------------
}