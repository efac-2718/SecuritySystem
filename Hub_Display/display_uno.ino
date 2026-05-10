#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <Fonts/FreeSansBold12pt7b.h>

MCUFRIEND_kbv tft;

// ================= COLORS =================
#define BG_COLOR      0x0000
#define TITLE_BG      0x001F
#define GREEN_COLOR   0x07E0
#define RED_COLOR     0xF800
#define WHITE_COLOR   0xFFFF
#define BLACK_COLOR   0x0000
#define drak_color    0x4765

// ================= ROOM SETTINGS =================
const int TOTAL_ROOMS = 4;

// Room names
String roomNames[TOTAL_ROOMS] = {
  "101",
  "102",
  "103",
  "104"
};

// Room states
// NORMAL = Green
// EMERGENCY = Red
String roomStatus[TOTAL_ROOMS] = {
  "NORMAL",
  "NORMAL",
  "NORMAL",
  "NORMAL"
};

// =================================================
// DRAW ROOM BOX
// =================================================
void drawRoom(int index) {

  // 2 x 2 Layout
  int row = index / 2;
  int col = index % 2;

  int boxW = 220;
  int boxH = 100;

  int x = 15 + (col * 235);
  int y = 90 + (row * 120);

  uint16_t fillColor;

  // Emergency = RED
  if (roomStatus[index] == "EMERGENCY") {
    fillColor = RED_COLOR;
  }
  // Normal = GREEN
  else {
    fillColor = drak_color;
  }

  // FULL COLOR BOX
  tft.fillRect(x, y, boxW, boxH, fillColor);

  // White Border
  tft.drawRect(x, y, boxW, boxH, WHITE_COLOR);
  tft.drawRect(x + 1, y + 1, boxW - 2, boxH - 2, WHITE_COLOR);

  // Text
  tft.setTextColor(WHITE_COLOR);
  tft.setTextSize(3);

  tft.setCursor(x + 20, y + 25);
  tft.print("ROOM");

  tft.setTextSize(5);
  tft.setCursor(x + 45, y + 55);
  tft.print(roomNames[index]);
}

// =================================================
// SETUP
// =================================================
void setup() {

  Serial.begin(9600);

  pinMode(A4, OUTPUT);

  digitalWrite(A4, LOW);
  delay(100);

  digitalWrite(A4, HIGH);
  delay(100);

  uint16_t ID = tft.readID();

  if (ID == 0xD3D3 || ID == 0x0000) {
    ID = 0x9486;
  }

  tft.begin(ID);

  tft.setRotation(1);

  // Background
  tft.fillScreen(BG_COLOR);

  // ================= TITLE BAR =================
  tft.fillRect(0, 0, 480, 60, TITLE_BG);

  tft.drawFastHLine(0, 60, 480, WHITE_COLOR);

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(WHITE_COLOR);

  tft.setCursor(30, 40);
  tft.print("        EMERGENCY MONITOR         ");

  tft.setFont(NULL);

  // Draw All Rooms
  for (int i = 0; i < TOTAL_ROOMS; i++) {
    drawRoom(i);
  }
}

// =================================================
// LOOP
// =================================================
void loop() {

  // Serial Format Examples:
  //
  // 101:EMERGENCY
  // 101:RESET
  //
  // RESET changes room back to GREEN

  if (Serial.available()) {

    String input = Serial.readStringUntil('\n');

    input.trim();

    int colonPos = input.indexOf(':');

    if (colonPos != -1) {

      String room = input.substring(0, colonPos);

      String command = input.substring(colonPos + 1);

      command.trim();

      for (int i = 0; i < TOTAL_ROOMS; i++) {

        if (roomNames[i] == room) {

          // Emergency -> RED
          if (command == "EMERGENCY") {
            roomStatus[i] = "EMERGENCY";
          }

          // Reset -> GREEN
          else if (command == "RESET") {
            roomStatus[i] = "NORMAL";
          }

          drawRoom(i);

          break;
        }
      }
    }
  }
}
