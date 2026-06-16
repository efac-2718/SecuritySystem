#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <Fonts/FreeSansBold12pt7b.h>

MCUFRIEND_kbv tft;

// --- Modern Color Palette ---
#define BG_COLOR      0x006F  // Deep Blue (#000F7D) requested
#define TITLE_BG      0x0000  // Deep Black Header
#define RED_COLOR     0xE000  // Vibrant Red
#define GREEN_COLOR   0x05E0  // Bright Green
#define GRAY_COLOR    0x39E7  // Subtle Gray
#define WHITE_COLOR   0xFFFF  // White
#define HR_LINE_COLOR 0xFD20  // Orange/Gold accent line
#define BORDER_COLOR  0xFFFF  // White borders
#define ICON_COLOR    0xFFE0  // Bright Yellow for the fire icon

const int TOTAL_ROOMS = 4;
String roomNames[TOTAL_ROOMS] = {"101", "102", "103", "104"};
String roomStatus[TOTAL_ROOMS] = {"OFFLINE", "OFFLINE", "OFFLINE", "OFFLINE"};

// 32x32 Fire Emoji Bitmap (Using uint8_t for strict memory compliance)
const uint8_t fire_icon [] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0f, 0x80, 0x00,
  0x00, 0x1f, 0xc0, 0x00, 0x00, 0x3f, 0xc0, 0x00, 0x00, 0x7f, 0xe0, 0x00,
  0x00, 0xff, 0xe0, 0x00, 0x01, 0xff, 0xf0, 0x00, 0x03, 0xff, 0xf0, 0x00,
  0x07, 0xff, 0xf8, 0x00, 0x0f, 0xdf, 0xf8, 0x00, 0x0f, 0x8f, 0xfc, 0x00,
  0x1f, 0x07, 0xfc, 0x00, 0x3e, 0x03, 0xfe, 0x00, 0x3c, 0x03, 0xfe, 0x00,
  0x78, 0x01, 0xff, 0x00, 0x78, 0x01, 0xff, 0x00, 0xf0, 0x00, 0xff, 0x80,
  0xf0, 0x38, 0x7f, 0x80, 0xf0, 0x7c, 0x3f, 0x80, 0xf0, 0xfe, 0x3f, 0x80,
  0xf1, 0xff, 0x1f, 0x80, 0xf1, 0xff, 0x1f, 0x80, 0x79, 0xff, 0x1f, 0x00,
  0x7b, 0xff, 0x8f, 0x00, 0x3f, 0xff, 0xce, 0x00, 0x3f, 0xff, 0xfc, 0x00,
  0x1f, 0xff, 0xf8, 0x00, 0x0f, 0xff, 0xf0, 0x00, 0x07, 0xff, 0xe0, 0x00,
  0x03, 0xff, 0xc0, 0x00, 0x00, 0xff, 0x00, 0x00
};

void drawRoom(int index) {
  int row = index / 2;
  int col = index % 2;
  int x = 15 + (col * 235);
  int y = 90 + (row * 120);

  uint16_t fillColor;
  if (roomStatus[index] == "EMERGENCY") fillColor = RED_COLOR;
  else if (roomStatus[index] == "NORMAL") fillColor = GREEN_COLOR;
  else fillColor = GRAY_COLOR; 

  // Smooth rounded corners
  tft.fillRoundRect(x, y, 220, 100, 10, fillColor);
  tft.drawRoundRect(x, y, 220, 100, 10, BORDER_COLOR);

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(WHITE_COLOR);
  
  tft.setTextSize(1);
  tft.setCursor(x + 20, y + 35);
  tft.print("ROOM");
  
  tft.setTextSize(2);
  tft.setCursor(x + 40, y + 85);
  tft.print(roomNames[index]);

  // Draw bright Yellow Fire Icon on Red background (fixes transparency bug)
  if (roomStatus[index] == "EMERGENCY") {
    tft.drawBitmap(x + 175, y + 15, (const uint8_t*)fire_icon, 32, 32, ICON_COLOR, RED_COLOR);
  }
}

void setup() {
  Serial.begin(9600); 
  
  uint16_t ID = tft.readID();
  if (ID == 0xD3D3 || ID == 0x0000) ID = 0x9486;
  
  tft.begin(ID);
  tft.setRotation(1);
  tft.fillScreen(BG_COLOR);

  tft.fillRect(0, 0, 480, 55, TITLE_BG);
  tft.fillRect(0, 55, 480, 4, HR_LINE_COLOR);

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(WHITE_COLOR);
  tft.setTextSize(1);
  tft.setCursor(110, 35); 
  tft.print("EMERGENCY MONITOR");

  for (int i = 0; i < TOTAL_ROOMS; i++) drawRoom(i);
}

void loop() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    int colonPos = input.indexOf(':');
    if (colonPos != -1) {
      String room = input.substring(0, colonPos);
      String command = input.substring(colonPos + 1);

      for (int i = 0; i < TOTAL_ROOMS; i++) {
        if (roomNames[i] == room) {
          if (command == "EMERGENCY") roomStatus[i] = "EMERGENCY";
          else if (command == "RESET") roomStatus[i] = "NORMAL";
          else if (command == "OFFLINE") roomStatus[i] = "OFFLINE";
          else if (command == "ONLINE") roomStatus[i] = "NORMAL";
          
          drawRoom(i); 
        }
      }
    }
  }
}