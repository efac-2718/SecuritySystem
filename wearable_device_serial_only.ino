/*
 * Project: Wrist-Worn Fall Detector (No OLED + Original FSM + LED)
 * Hardware: ESP32-C3 Super Mini + MPU6050
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// ================= MAC ADDRESS =================
uint8_t receiverAddress[] = {0x68, 0xFE, 0x71, 0x0B, 0xB5, 0xD8};

// ================= PINS =================
#define I2C_SDA 8
#define I2C_SCL 9
#define BTN_PIN 1       
#define LED_PIN 5
#define MPU_INT_PIN 4  
#define GRAVITY_MS2 9.80665f

// ================= THRESHOLDS =================
const float THRESHOLD_FREEFALL_G = 0.3f;
const unsigned long WINDOW_FREEFALL_MS = 600; 
const float THRESHOLD_IMPACT_G = 3.0f; 
const float THRESHOLD_ANGLE_CHANGE = 90.0f;
const unsigned long WINDOW_POSTURE_MS = 4000; 
const float THRESHOLD_AVG_ACTIVITY = 60.0f;
const float THRESHOLD_WAKE_MOTION = 10.0f; 

// ================= VARIABLES =================
typedef struct struct_message {
  char msg[32];
  float g_force;
  char  employee_id[16];
} struct_message;

struct_message packetData;
esp_now_peer_info_t peerInfo;
volatile bool ackReceived = false;

Adafruit_MPU6050 mpu;

enum SystemState { 
  STATE_MONITORING, 
  STATE_FREEFALL_WAIT, 
  STATE_POSTURE_CHECK, 
  STATE_SENDING_ALERT,   
  STATE_ALERT_CONFIRMED  
};
SystemState currentState = STATE_MONITORING;
SystemState lastState = STATE_ALERT_CONFIRMED;

unsigned long stateTimer = 0;
unsigned long lastLoopTime = 0;
unsigned long lastPacketTime = 0;
unsigned long ledTimer = 0; 
unsigned long debugTimer = 0;
unsigned long sleepTimer = 0;
const unsigned long INACTIVITY_TIMEOUT = 15000; 

unsigned long btnHoldTimer = 0;
const unsigned long RESET_HOLD_TIME = 5000; 

float pitch = 0, roll = 0;
float ref_pitch = 0, ref_roll = 0;
float last_svm = 0;
float total_gyro_mag = 0;
int sample_count = 0;
bool ledStateHigh = false; 

// ================= CHANNEL SCAN =================
int32_t getHotspotChannel(const char *target_ssid) {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == target_ssid) {
      return WiFi.channel(i);
    }
  }
  Serial.println(">> WARN: Hotspot not found, defaulting to channel 1");
  return 1;
}

// ================= CALLBACKS =================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    ackReceived = true;
    Serial.println(">> RADIO: Ack Received!");
  }
}

// ================= SLEEP =================
void enterDeepSleep() {
  Serial.println(">> POWER: Entering Deep Sleep...");
  Serial.flush();

  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  mpu.setHighPassFilter(MPU6050_HIGHPASS_0_63_HZ);
  mpu.setMotionDetectionThreshold(20); 
  mpu.setMotionDetectionDuration(2);
  mpu.setInterruptPinLatch(true);
  mpu.setInterruptPinPolarity(false); 
  mpu.setMotionInterrupt(true);

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  delay(10); 

  digitalWrite(LED_PIN, LOW);

  esp_deep_sleep_enable_gpio_wakeup((1ULL << MPU_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_enable_gpio_wakeup((1ULL << BTN_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);

  esp_deep_sleep_start();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- BOOTING ---");

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(MPU_INT_PIN, INPUT_PULLDOWN);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  
  if (!mpu.begin()) {
    Serial.println("!! ERROR: MPU6050 Missing !!");
    while (1) { digitalWrite(LED_PIN, HIGH); delay(100); digitalWrite(LED_PIN, LOW); delay(100); }
  }
  Serial.println("MPU6050 Ready.");

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G); 
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  // --- CHANNEL SCAN ---
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println(">> RADIO: Scanning for hub channel...");
  int32_t dynamicChannel = getHotspotChannel("Dialog 4G 136");
  Serial.print(">> RADIO: Aligning to channel "); Serial.println(dynamicChannel);

  esp_wifi_set_channel(dynamicChannel, WIFI_SECOND_CHAN_NONE);

  // --- ESP-NOW INIT ---
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW Error"); return; }
  esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);
  
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel = dynamicChannel; // matched dynamically
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) return;

  // --- WAKE UP CHECK ---
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
     Serial.println(">> SYSTEM: Cold Boot. Arming...");
     delay(2000);
     enterDeepSleep();
  }

  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
     if (digitalRead(BTN_PIN) == LOW) {
        Serial.println(">> WAKEUP: Manual SOS!");
        currentState = STATE_SENDING_ALERT;
        last_svm = 99.9; 
     } else {
        Serial.println(">> WAKEUP: Motion Detected.");
        currentState = STATE_MONITORING;
     }
  }

  lastLoopTime = millis();
  sleepTimer = millis(); 
}

// ================= LOOP =================
void loop() {
  unsigned long currentTime = millis();
  float dt = (currentTime - lastLoopTime) / 1000.0;
  lastLoopTime = currentTime;

  if (currentState == STATE_ALERT_CONFIRMED) {
      if (digitalRead(BTN_PIN) == LOW) {
          if (btnHoldTimer == 0) {
            btnHoldTimer = millis(); 
            Serial.println(">> INPUT: Holding Reset...");
          }
          if (millis() - btnHoldTimer > RESET_HOLD_TIME) {
              Serial.println(">> ACTION: RESETTING!");
              digitalWrite(LED_PIN, LOW); 
              enterDeepSleep(); 
              return; 
          }
      } else {
          btnHoldTimer = 0; 
      }
  }

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float ax = a.acceleration.x / GRAVITY_MS2;
  float ay = a.acceleration.y / GRAVITY_MS2;
  float az = a.acceleration.z / GRAVITY_MS2;
  float svm = sqrt(sq(ax) + sq(ay) + sq(az));
  last_svm = svm;

  float gx_deg = g.gyro.x * 57.2958;
  float gy_deg = g.gyro.y * 57.2958;
  float gz_deg = g.gyro.z * 57.2958;
  float current_gyro_mag = sqrt(sq(gx_deg) + sq(gy_deg) + sq(gz_deg));

  float accel_pitch = atan2(ay, az) * 180.0 / PI;
  float accel_roll = atan2(-ax, sqrt(ay*ay + az*az)) * 180.0 / PI;
  pitch = 0.96 * (pitch + gx_deg * dt) + 0.04 * accel_pitch;
  roll  = 0.96 * (roll  + gy_deg * dt) + 0.04 * accel_roll;

  if (currentState != lastState) {
      Serial.print(">> STATE CHANGE: "); Serial.println(currentState);
      lastState = currentState;
  }

  switch (currentState) {
    case STATE_MONITORING:
      ref_pitch = pitch;
      ref_roll = roll;
      ackReceived = false; 
      
      if (svm < THRESHOLD_FREEFALL_G) {
        Serial.print(">> EVENT: Freefall! SVM="); Serial.println(svm);
        stateTimer = millis();
        currentState = STATE_FREEFALL_WAIT;
        sleepTimer = millis(); 
      }
      
      if (current_gyro_mag > THRESHOLD_WAKE_MOTION) sleepTimer = millis();
      if (millis() - sleepTimer > INACTIVITY_TIMEOUT) enterDeepSleep();

      if (digitalRead(BTN_PIN) == LOW && btnHoldTimer == 0) {
         Serial.println(">> EVENT: Button SOS!");
         currentState = STATE_SENDING_ALERT;
         last_svm = 99.9;
         sleepTimer = millis();
      }
      break;

    case STATE_FREEFALL_WAIT:
      sleepTimer = millis(); 
      if (svm > THRESHOLD_IMPACT_G) {
        Serial.print(">> EVENT: Impact! SVM="); Serial.println(svm);
        stateTimer = millis();
        currentState = STATE_POSTURE_CHECK;
        total_gyro_mag = 0;
        sample_count = 0;
      }
      else if (millis() - stateTimer > WINDOW_FREEFALL_MS) {
        Serial.println(">> INFO: Freefall Timeout");
        currentState = STATE_MONITORING;
      }
      break;

    case STATE_POSTURE_CHECK:
      sleepTimer = millis(); 
      total_gyro_mag += current_gyro_mag; 
      sample_count++;
      
      if (millis() - stateTimer > WINDOW_POSTURE_MS) {
        float avg_activity = total_gyro_mag / sample_count;
        float delta_p = abs(pitch - ref_pitch);
        float delta_r = abs(roll - ref_roll);
        bool angle_changed = (delta_p > THRESHOLD_ANGLE_CHANGE || delta_r > THRESHOLD_ANGLE_CHANGE);

        Serial.print(">> CHECK: Activity="); Serial.print(avg_activity);
        Serial.print(" dPitch="); Serial.print(delta_p);
        Serial.print(" dRoll="); Serial.println(delta_r);

        if (avg_activity < THRESHOLD_AVG_ACTIVITY && angle_changed) {
           Serial.println(">> RESULT: FALL CONFIRMED!");
           currentState = STATE_SENDING_ALERT;
        } else {
           Serial.println(">> RESULT: Cancelled (Moved or Angle OK)");
           currentState = STATE_MONITORING;
        }
      }
      break;

    case STATE_SENDING_ALERT:
      sleepTimer = millis(); 
      manageLed(false);

      if (ackReceived) {
        currentState = STATE_ALERT_CONFIRMED;
      }
      else {
        if (millis() - lastPacketTime > 500) {
          Serial.println(">> RADIO: Sending SOS...");
          sendSOS();
          lastPacketTime = millis();
        }
      }
      break;

    case STATE_ALERT_CONFIRMED:
      sleepTimer = millis(); 
      manageLed(true);
      break;
  }

  printStatus();
}

// ================= HELPERS =================
void manageLed(bool confirmed) {
  if (confirmed) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    if (millis() - ledTimer > 100) {
      ledTimer = millis();
      ledStateHigh = !ledStateHigh;
      digitalWrite(LED_PIN, ledStateHigh ? HIGH : LOW);
    }
  }
}

void sendSOS() {
  strcpy(packetData.msg, "FALL_DETECTED");
  strcpy(packetData.employee_id, "EMP-002");
  packetData.g_force = last_svm;
  esp_now_send(receiverAddress, (uint8_t *) &packetData, sizeof(packetData));
}

void printStatus() {
  if (millis() - debugTimer > 2000) {
    debugTimer = millis();
    if (currentState == STATE_MONITORING) {
        Serial.print("STATUS: Active | SVM: "); Serial.print(last_svm);
        Serial.print(" | SleepIn: "); Serial.println((INACTIVITY_TIMEOUT - (millis() - sleepTimer))/1000);
    } else if (currentState == STATE_ALERT_CONFIRMED) {
        Serial.println("STATUS: Help Sent. Hold Button to Reset.");
    }
  }
}