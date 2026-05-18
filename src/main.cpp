#include <Arduino.h>
#include <Wire.h> 
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>

// --- PIN DEFINITIONS ---
#define GSM_RX_PIN 16 
#define GSM_TX_PIN 17 
#define GSM_BAUD_RATE 9600 

// Buttons & Outputs
#define BTN_ENTER 34
#define BTN_RESET 35 
#define BTN_SENSOR 4
#define PIN_BUZZER 32
#define PIN_LED 2

Preferences preferences;

// --- KEYPAD CONFIGURATION ---
const byte ROWS = 4; 
const byte COLS = 3; 
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[ROWS] = {13, 12, 14, 27}; 
byte colPins[COLS] = {26, 25, 33}; 

HardwareSerial sim900(2);

String contact1;
String contact2;
String emergency;
String correctPin; // Fixed: Only declared once here

String confirmMessage = "Hi. This message confirms that you are taking action.";
bool alarmDisarm = true;
unsigned long alarmStartTime = 0;   
unsigned long previousBlinkMillis = 0; 
int ledState = LOW; 

String currentInput = "";
bool alarmActive = false;

// --- FALL DETECTION STRUCTURE ---
typedef struct struct_message {
  char msg[32];
  float g_force;
} struct_message;

struct_message incomingPacket;
volatile bool fallSignalReceived = false;
float receivedGForce = 0;

// --- FUNCTION PROTOTYPES ---
void blinkLED();
void formPin();  
void checkEnterButton(); 
void checkResetButton();
void updateLCD(String line1, String line2);
int emergencyContact1();
int emergencyContact2();
void emergencyServices();
void clearSerialBuffer();
void sendSMS(String number, String message);
void idleState();
void initFallDetection();
void handleEmergencyCalls();
void runSetupMode(); 
String readKeypadInput(String prompt, String currentVal);

// --- FALL DETECTION CALLBACK ---
// NOTE: This signature is for ESP32 Board Manager v3.0.0+
// If you get an error here, change 'const esp_now_recv_info *info' to 'const uint8_t * mac'
void OnFallDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  memcpy(&incomingPacket, incomingData, sizeof(incomingPacket));
  if (strcmp(incomingPacket.msg, "FALL_DETECTED") == 0) {
    fallSignalReceived = true;
    receivedGForce = incomingPacket.g_force;
  }
}

void initFallDetection() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnFallDataRecv);
  Serial.println("Fall Detection Active.");
  Serial.print("Device MAC Address: ");
  Serial.println(WiFi.macAddress());
}

void triggerEmergencyProtocol() {
  Serial.println("\n!!! CRITICAL ALERT: FALL DETECTED !!!");
  Serial.print("Impact Force: "); Serial.print(receivedGForce); Serial.println(" G");

  if(alarmActive == false ){
    alarmActive = true;
    alarmStartTime = millis();
    alarmDisarm = true; // Wait for PIN first
    currentInput = ""; 
    Serial.println("ALARM TRIGGERED! PIN Entry Phase.");
    updateLCD("ALARM!", "Enter PIN:");
  }
}

void handleAlarmActions() {
    if (!alarmActive) return; 

    formPin();
    checkResetButton(); 

    // Countdown Timer (45 seconds)
    if (millis() - alarmStartTime >= 5000) {
      // If alarm is still armed (user didn't enter PIN)
      if (alarmDisarm) { 
        Serial.println("TIME EXPIRED: Moving to Call Protocol.");
        updateLCD("Time Expired", "Init Call");
        digitalWrite(PIN_LED, LOW);     // Fixed Variable Name
        noTone(PIN_BUZZER);             // Fixed Variable Name
        
        alarmDisarm = false; // Enable Calling
        alarmActive = false; // Stop checking PIN
        
        handleEmergencyCalls(); 
      }
    }
}

void handleEmergencyCalls() {
    int keyPressed1 = emergencyContact1(); 
    Serial.println(String(keyPressed1));
    
    // Outcome 1: User Pressed '1' (Cancel/Confirm)
    if(keyPressed1 == 1 ) {
        delay(4000);
        sendSMS(contact1, confirmMessage);
        idleState();
        return;
    }

    // Outcome 2: User Pressed '2' OR Timeout
    if(keyPressed1 == 2 || keyPressed1 == -1) {
        delay(4000);
        int keyPressed2 = emergencyContact2(); 
        Serial.println(String(keyPressed2));
        
        if(keyPressed2 == 1) {
            delay(4000);
            sendSMS(contact2, confirmMessage);
            idleState();
            return;
        }
        
        if(keyPressed2 == 2 || keyPressed2 == -1) {
            delay(4000);
            emergencyServices(); 
            idleState();
            return;
        }
    }

    // Outcome 3: Immediate Emergency
    if(keyPressed1 == 3) {
        delay(4000);
        emergencyServices(); 
        idleState();
        return;
    }
}

void checkFallStatus() {
  if (fallSignalReceived) {
    fallSignalReceived = false; 
    triggerEmergencyProtocol(); 
  }
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

void setup() {
  Serial.begin(9600);
  sim900.begin(GSM_BAUD_RATE, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  
  pinMode(PIN_LED, OUTPUT);
  pinMode(BTN_ENTER, INPUT); 
  pinMode(BTN_RESET, INPUT); 
  pinMode(BTN_SENSOR, INPUT); 
  pinMode(PIN_BUZZER, OUTPUT);
  
  
  // --- LOAD SETTINGS FROM MEMORY ---
  preferences.begin("system-config", false); 
  contact1 = preferences.getString("c1", "+94764175179"); 
  contact2 = preferences.getString("c2", "+94764175179");
  emergency = preferences.getString("em", "+94764175179");
  correctPin = preferences.getString("pin", "1234");
  preferences.end();
  
  Serial.println("Settings Loaded:");
  Serial.println("C1: " + contact1);
  Serial.println("PIN: " + correctPin);

  // --- CHECK FOR SETUP MODE ---
  if(digitalRead(BTN_RESET) == HIGH) {
    Serial.println("Entering Setup Mode...");
    runSetupMode();
  }

  // --- NORMAL STARTUP ---
  clearSerialBuffer();
  Serial.println("Emergency Call System Starting...");
  
  delay(10000); 
  
  sim900.println("AT+CFSTERM");
  checkResponse("OK", 3000); 
  sim900.println("AT+CFSINIT");
  checkResponse("OK", 3000);
  sim900.println("AT+DDET=1");
  checkResponse("OK", 2000);
  sim900.println("AT+CLCC=1"); 
  checkResponse("OK", 1000);

  Serial.println("System Ready.");
  idleState(); 
  initFallDetection();
}

void loop() {
  checkFallStatus();
  
  // Also check Physical button (Pin 4)
  if(digitalRead(BTN_SENSOR) == HIGH && alarmActive == false) {
     triggerEmergencyProtocol();
  }
  
  if (alarmActive) {
    blinkLED();
  }
  
  handleAlarmActions(); 
}



int emergencyContact1() {
  int keyPressed = -1;
  int retry = 0;
  while (retry < 2) {
    bool callActive = false;
    sim900.println("ATD" + contact1 + ";");
    Serial.println("Calling contact 1");
    updateLCD("Calling emergency","contact 1");
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
  updateLCD("Transferring to","contact 2" );
  return -1;
}

int emergencyContact2() {
  int keyPressed = -1;
  int retry = 0;
  while (retry < 2) {
    bool callActive = false;
    sim900.println("ATD" + contact2 + ";");
    updateLCD("Calling emergency","contact 2");
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
  updateLCD("Transferring to","ES" );
  return -1;
}

void emergencyServices(){
  Serial.println("Calling emergency services");
  updateLCD("Calling emergen-","cy services");
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

void idleState() {
  updateLCD("Monitoring", "Patient ..."); 
  Serial.println("System ready , Monitoring patient");
  clearSerialBuffer(); 
  alarmDisarm = true; 
  alarmActive = false;
  currentInput = "";
}

void clearSerialBuffer() {
  while(sim900.available()) sim900.read();
}

void sendSMS(String number, String message) {
  Serial.println("Sending SMS message...");
  updateLCD("Sending confirm","message");
  sim900.println("AT+CMGF=1"); 
  delay(4000);

  sim900.println("AT+CMGS=\"" + number + "\"");
  while(sim900.available()){
    String response = sim900.readString();
    response.trim();
    if(response.indexOf(">") != -1){
      delay(3000);
      sim900.print(message);
      sim900.write(26);
    }
  }
}