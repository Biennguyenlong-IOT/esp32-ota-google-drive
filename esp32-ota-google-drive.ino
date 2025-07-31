
#include <ChronosESP32.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <DHT.h>
#include "bluetooth.h"
#include "all_frames.h"

#define DHTPIN 13
#define DHTTYPE DHT22
#define SCREEN_WIDTH 128
#define LINE_HEIGHT 12
#define DHT_UPDATE_INTERVAL 5000
#define FRAME_DELAY 42    // ≈24 FPS

DHT dht(DHTPIN, DHTTYPE);
ChronosESP32 watch("Chronos Nav - AM-032");

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R2, U8X8_PIN_NONE, 4, 5); // (R2 là quay 180 độ, không có chân reset, scl,sda)

bool notificationDisplayed = false;  // Thêm biến này ở global
bool newNotification = false;
bool isIncomingCall = false;
bool change = false;
bool rotate = false, flip = false;
float temp = NAN, humi = NAN;
unsigned long lastDHTUpdate = 0;
unsigned long notificationTimestamp = 0;

enum DisplayState {
  SHOW_NAVIGATION,
  SHOW_NOTIFICATION,
  SHOW_CLOCK,
  SHOW_DASAI,
  SHOW_WEATHER
};
DisplayState currentState = SHOW_CLOCK;
DisplayState previousState = SHOW_CLOCK;

void connectionCallback(bool state) {
  Serial.printf("Connection state: %s\n", state ? "Connected" : "Disconnected");
}
unsigned long stateStartTime = 0;  // ghi thời điểm vào mỗi trạng thái
const unsigned long NAV_DISPLAY_DURATION = 10000;  // 10 giây
const unsigned long NOTIF_DISPLAY_DURATION = 10000;  // 10 giây

// =========  Biến toàn cục bổ sung  =========
String  lastDirections  = "";
// String  lastSpeed      = "";
String  lastTitle       = "";
String  lastDistance    = "";
uint16_t nav_crc        = 0;
bool     isNavActive    = false;
Navigation lastNav;
bool firstNavShown = false;
// =========  Hàm vẽ icon 48×48  =========
void drawIcon48x48(const uint8_t *icon, int xOff, int yOff) {
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 48; x++)
    {
      int idx   = (y * 48 + x) >> 3;
      int bit   = 7 - (x & 0x07);
      if ((icon[idx] >> bit) & 0x01)
        display.drawPixel(xOff + x, yOff + y);
    }
}
/* =========================================================
   Hàm dùng chung để in đoạn văn UTF-8 tự động xuống dòng
   =========================================================*/
void drawUtf8Wrapped(const String &text, int x, int y, int maxWidth, int lineHeight) {
  String line = "";
  String word = "";

  for (int i = 0; i <= text.length();) {
    // Xử lý cuối chuỗi
    if (i == text.length()) {
      if (word.length()) {
        String tmp = line.length() ? line + " " + word : word;
        if (display.getUTF8Width(tmp.c_str()) > maxWidth) {
          if (line.length()) {
            display.drawUTF8(x, y, line.c_str());
            y += lineHeight;
          }
          display.drawUTF8(x, y, word.c_str());
          y += lineHeight;
        } else {
          line = tmp;
          display.drawUTF8(x, y, line.c_str());
          y += lineHeight;
        }
      } else if (line.length()) {
        display.drawUTF8(x, y, line.c_str());
        y += lineHeight;
      }
      break;
    }

    // Lấy ký tự UTF-8
    uint8_t c = text[i];
    String ch;
    if (c == ' ') {
      // Kiểm tra dòng mới nếu cần
      String tmp = line.length() ? line + " " + word : word;
      if (display.getUTF8Width(tmp.c_str()) > maxWidth) {
        if (line.length()) {
          display.drawUTF8(x, y, line.c_str());
          y += lineHeight;
        }
        line = word;
      } else {
        line = tmp;
      }
      word = "";
      i++;
      continue;
    }

    if (c < 128) {
      ch += text[i++];
    } else if ((c & 0xE0) == 0xC0) {
      ch = text.substring(i, i + 2); i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      ch = text.substring(i, i + 3); i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      ch = text.substring(i, i + 4); i += 4;
    }

    word += ch;
  }
}

void displayNavigationFull(const Navigation &nav) {
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  // Icon trái
  drawIcon48x48(nav.icon, 0, 12);
  // Xoá dòng đầu (Title + ETA)
  display.setDrawColor(0);
  // display.drawBox(0, 64, 50, 12);
  display.drawBox(0, 0, 128, 12);
  display.setDrawColor(1);
    // Hiển thị tốc độ
  String speedStr = nav.speed.length() ? "SP: " + nav.speed : "SP: -- km/h";
  display.drawUTF8(0, 11, speedStr.c_str());
  // Serial.println(nav.speed);
  // ETA căn phải
  String etaStr = "ETA: ??";
  if (nav.eta.length()) {
    int index = nav.eta.lastIndexOf(' ');
    if (index != -1 && index + 1 < nav.eta.length()) {
      String timeOnly = nav.eta.substring(index + 1);  // "09:30"
      etaStr = "ETA: " + timeOnly;
    }
  }
  // int etaWidth = display.getUTF8Width(etaStr.c_str());
  // display.drawUTF8(128 - etaWidth, 11, etaStr.c_str());
  display.drawUTF8(75, 11, etaStr.c_str());
    // Xoá vùng title (dòng cuối cùng)
  display.setDrawColor(0);
  display.drawBox(0, 56, 128, 12);  // y = 56~67
  display.setDrawColor(1);
  // Title
  String titleStr = nav.title.length() ? nav.title : "--";
  display.drawUTF8(0, 64, titleStr.c_str());
  display.setFont(u8g2_font_unifont_t_vietnamese1);
  

  // Directions
  int textX = 50;
  int yStart = 27;
  int maxWidth = 128 - textX;
  int lineHeight = 12;
  String directions = nav.directions.length() ? nav.directions : "Không có chỉ dẫn";
  drawUtf8Wrapped(directions, textX, yStart, maxWidth, lineHeight);

  display.sendBuffer();
}
void displayNavigationPartial(const Navigation &nav) {
  // display.setFont(u8g2_font_unifont_t_vietnamese1);
  display.setFont(u8g2_font_ncenB08_tr);
  // Xoá dòng đầu (Title + ETA)
  display.setDrawColor(0);
  display.drawBox(0, 0, 128, 12);
  // display.drawBox(0, 64, 50, 12);
  display.setDrawColor(1);

  String speedStr = nav.speed.length() ? "SP: " + nav.speed : "SP: -- km/h";
  display.drawUTF8(0, 11, speedStr.c_str());

  // ETA căn phải
    String etaStr = "ETA: ??";
    if (nav.eta.length()) {
    int index = nav.eta.lastIndexOf(' ');
    if (index != -1 && index + 1 < nav.eta.length()) {
      String timeOnly = nav.eta.substring(index + 1);  // "09:30"
      etaStr = "ETA: " + timeOnly;
    }
  }
  // int etaWidth = display.getUTF8Width(etaStr.c_str());
  // display.drawUTF8(128 - etaWidth, 11, etaStr.c_str());
    
    display.drawUTF8(75, 11, etaStr.c_str());
    // Xoá và cập nhật title
    display.setDrawColor(0);
    display.drawBox(0, 56, 128, 12);
    display.setDrawColor(1);
      // Title
    String titleStr = nav.title.length() ? nav.title : "--";
    display.drawUTF8(0, 64, titleStr.c_str());
    display.setFont(u8g2_font_unifont_t_vietnamese1);

  // Xoá vùng directions bên phải icon
  int textX = 50;
  int yStart = 27;
  int maxWidth = 128 - textX;
  int lineHeight = 12;
  display.setDrawColor(0);
  display.drawBox(textX, yStart, maxWidth, 64 - yStart);
  display.setDrawColor(1);

  // Vẽ lại directions
  String directions = nav.directions.length() ? nav.directions : "Không có chỉ dẫn";
  drawUtf8Wrapped(directions, textX, yStart, maxWidth, lineHeight);
  display.sendBuffer();
}
/* =========================================================
   Hàm này CHỈ được gọi khi icon thay đổi (CRC khác),
   vẽ lại toàn bộ (xoá sạch, vẽ icon + speed + directions)
   =========================================================
void displayNavigationFull(const Navigation &nav) {
  display.clearBuffer();
  display.setFont(u8g2_font_unifont_t_vietnamese1);

  // 1️⃣ Title – trên cùng
  String titleStr = nav.title.length() ? nav.title : "Đang dẫn đường";
  display.drawUTF8(0, 11, titleStr.c_str());

  // 2️⃣ Distance – dòng kế tiếp (dòng 2)
  String distStr = nav.distance.length() ? nav.distance : "0 m";
  int    distW   = display.getUTF8Width(distStr.c_str());
  display.drawUTF8(80, 11, distStr.c_str());

  // 3️⃣ Icon – nằm bên trái, lệch dưới 1 dòng (bên dưới title + distance)
  drawIcon48x48(nav.icon, 0, 11); // offset Y = 22 (sau 2 dòng text)

  // 4️⃣ Directions – từ dòng 3 trở đi
  int textX      = 50;
  int y          = 25;  // 11*3 = 33
  int maxWidth   = 128 - textX;
  int lineHeight = 11;
  String txt     = nav.directions.length() ? nav.directions : "Không có chỉ dẫn";

  String line = "";
  for (int i = 0; i < txt.length();) {
    String word = "";
    uint8_t c = txt[i];
    if (c < 128) { word += txt[i++]; }
    else if ((c & 0xE0) == 0xC0) { word += txt.substring(i, i + 2); i += 2; }
    else if ((c & 0xF0) == 0xE0) { word += txt.substring(i, i + 3); i += 3; }
    else if ((c & 0xF8) == 0xF0) { word += txt.substring(i, i + 4); i += 4; }

    if (word == " ") { line += " "; continue; }

    String tmp = line + word;
    if (display.getUTF8Width(tmp.c_str()) > maxWidth) {
      display.drawUTF8(textX, y, line.c_str());
      y += lineHeight;
      line = word;
    } else line = tmp;
  }
  if (line.length()) display.drawUTF8(textX, y, line.c_str());

  display.sendBuffer();
}
*/
/* =========================================================
   Hàm này chỉ vẽ lại speed + directions,
   KHÔNG xoá icon (để tránh nhấp nháy)
   =========================================================
void displayNavigationPartial(const Navigation &nav) {
  display.setFont(u8g2_font_unifont_t_vietnamese1);

  /* 1️⃣  SPEED – xoá riêng dòng 1 rồi vẽ lại 
  display.setDrawColor(0);
  display.drawBox(0, 0, 128, 11);          // clear dòng speed
  display.setDrawColor(1);

  String titleStr = nav.title.length() ? nav.title : "Đang dẫn đường";
  display.drawUTF8(0, 11, titleStr.c_str());

  String distStr = nav.distance.length() ? nav.distance : "0 m";
  int    distW   = display.getUTF8Width(distStr.c_str());
  // display.drawUTF8(128 - distW - 2, 11, distStr.c_str());
  // int distX = (128 - distW) / 2;  // ⭐ căn giữa dòng đầu
  display.drawUTF8(80, 11, distStr.c_str());
  
  /* 2️⃣  DIRECTIONS – xoá vùng text rồi vẽ 
  int textX      = 50;
  int yStart     = 22;
  int maxWidth   = 128 - textX;
  int lineHeight = 11;

  display.setDrawColor(0);
  display.drawBox(textX, yStart - lineHeight, maxWidth, 64 - yStart + lineHeight);
  display.setDrawColor(1);

  String txt  = nav.directions.length() ? nav.directions : "Không có chỉ dẫn";
  String line = "";
  int    y    = yStart;

  for (int i = 0; i < txt.length();)
  {
    String word = "";
    uint8_t c = txt[i];
    if (c < 128)                     { word += txt[i++]; }
    else if ((c & 0xE0) == 0xC0)     { word += txt.substring(i, i + 2); i += 2; }
    else if ((c & 0xF0) == 0xE0)     { word += txt.substring(i, i + 3); i += 3; }
    else if ((c & 0xF8) == 0xF0)     { word += txt.substring(i, i + 4); i += 4; }

    if (word == " ") { line += " "; continue; }

    String tmp = line + word;
    if (display.getUTF8Width(tmp.c_str()) > maxWidth)
    {
      display.drawUTF8(textX, y, line.c_str());
      y   += lineHeight;
      line = word;
    }
    else line = tmp;
  }
  if (line.length()) display.drawUTF8(textX, y, line.c_str());

  display.sendBuffer();
}
*/
/* =========================================================
   CALLBACK nhận dữ liệu từ Chronos / Android / v.v.
   =========================================================*/
void configCallback(Config cfg, uint32_t a, uint32_t b) {
  switch (cfg)
  {
    /* ---------- NAV DATA: directions + speed ---------- */
    case CF_NAV_DATA:
    {
      isNavActive = a;
      if (!isNavActive)
      {
        display.clearBuffer(); display.sendBuffer();
        lastDirections = ""; lastDistance = ""; return;
      }

      Navigation nav = watch.getNavigation();          // hoặc API của bạn
      bool dirChanged   = (nav.directions != lastDirections);
      bool distChanged   = (nav.distance   != lastDistance);

      if (dirChanged || distChanged)
      {
        displayNavigationPartial(nav);                 // chỉ vẽ lại speed + text
        lastDirections = nav.directions;
        lastDistance   = nav.distance;
      }
      break;
    }

    /* ---------- NAV ICON: chỉ vẽ lại khi CRC đổi ---------- */
    case CF_NAV_ICON:
    {
      if (!isNavActive || a != 2) break;               // a==2: icon gửi xong
      Navigation nav = watch.getNavigation();

      if (nav_crc != nav.iconCRC)
      {
        nav_crc = nav.iconCRC;
        displayNavigationFull(nav);                    // vẽ lại icon + speed + text
        lastDirections = nav.directions;
        lastDistance   = nav.distance;
      }
      break;
    }
      case CF_USER:
      flip = ((b >> 24) & 0xFF) == 1;    // imperial normal, metric flipped
      rotate = ((b >> 8) & 0xFF) == 1;   // celsius normal, fahrenheit rotated

      if (flip) {
        currentState = SHOW_CLOCK;       // Ưu tiên flip
      } else {
        if (rotate) {
          currentState = SHOW_DASAI;
        } else {
          currentState = SHOW_WEATHER;
        }
      }
      break;

  }
}

void drawWrappedText(int x, int yStart, const String& text, int maxWidth) {
  int y = yStart;
  String line = "";

  for (unsigned int i = 0; i < text.length();) {
    String word = "";
    uint8_t c = text[i];

    // Xử lý ký tự UTF-8
    if (c < 128) {
      word += text[i++];
    } else if ((c & 0xE0) == 0xC0) {
      word += text.substring(i, i + 2); i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      word += text.substring(i, i + 3); i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      word += text.substring(i, i + 4); i += 4;
    }

    if (word == " ") {
      line += word;
      continue;
    }

    String temp = line + word;
    if (display.getUTF8Width(temp.c_str()) > maxWidth) {
      display.drawUTF8(x, y, line.c_str());
      y += LINE_HEIGHT;
      line = word;
    } else {
      line = temp;
    }
  }

  if (line.length() > 0) {
    display.drawUTF8(x, y, line.c_str());
  }
}
void notificationCallback(Notification notification) {
  if (isNavActive) return;  // Bỏ qua nếu đang điều hướng

  newNotification = true;
  notificationTimestamp = millis();
  isIncomingCall = false;

  String app = notification.app;
  String title = notification.title;
  String message = notification.message;

  // Cắt chuỗi nếu có dạng "14] ... "
  int pos = message.indexOf(']');
  if (pos != -1 && pos + 1 < message.length()) {
    message = message.substring(pos + 1);
    message.trim();
  }

  // Kiểm tra nếu là cuộc gọi đến
  if ((app.equalsIgnoreCase("phone") || app.equalsIgnoreCase("call")) &&
      (title.indexOf("Incoming") != -1 || message.indexOf("Calling") != -1 || message.indexOf("Cuộc gọi") != -1)) {
    isIncomingCall = true;
  }

  Serial.printf("Notification at %s\nFrom: %s\tIcon: %d\n%s\n%s\n",
                notification.time.c_str(),
                app.c_str(),
                notification.icon,
                title.c_str(),
                message.c_str());

  display.clearBuffer();
  display.setFont(u8g2_font_unifont_t_vietnamese1);

  if (isIncomingCall) {
    // Giao diện cuộc gọi
    display.drawBox(0, 0, 128, 64);        // Nền đen
    display.setDrawColor(0);              // Chữ trắng
    display.drawUTF8(5, 24, "CUỘC GỌI ĐẾN");

    String caller = title;
    caller.trim();
    if (caller.length() == 0) {
      caller = "Người lạ";
    }
    display.drawUTF8(5, 35, ("Người gọi: " + caller).c_str());
    display.setDrawColor(1);              // Reset lại màu bình thường

  } else {
    // Tin nhắn bình thường
    display.setCursor(0, 12);
    drawWrappedText(0, 12, message, 128);
  }

  display.sendBuffer();
}

void drawBluetoothAndBattery() {
  if (watch.isConnected()) {
    display.drawXBMP(0, -5, 24, 24, bluetooth);

    int batteryPercent = watch.getPhoneBattery();                     // lấy % pin
    int batteryLevel = map(batteryPercent, 0, 100, 0, 18);            // dùng cho biểu tượng

    // Vẽ khung pin
    display.drawFrame(110, 0, 18, 7);
    display.drawBox(110, 0, batteryLevel, 7);

    // Hiển thị phần trăm pin kế bên
    display.setFont(u8g2_font_5x7_tr);  // font nhỏ để vừa
    String percentStr = String(batteryPercent) + "%";
    display.drawStr(90, 7, percentStr.c_str());  // vẽ phía trước biểu tượng pin
  }
}
void drawTimeAndDate() {
  // Xóa vùng thời gian (center top)
  display.setDrawColor(0);
  display.drawBox(0, 24, SCREEN_WIDTH, 16);   // Tùy theo vị trí chữ
  display.drawBox(0, 43, SCREEN_WIDTH, 14);  // Xóa vùng ngày
  display.setDrawColor(1);

  display.setFont(u8g2_font_ncenB12_tr);
  String timeString = watch.getHourZ() + watch.getTime(":%M:%S");
  int textWidth = display.getStrWidth(timeString.c_str());
  display.setCursor((SCREEN_WIDTH - textWidth) / 2, 24);
  display.print(timeString);

  display.setFont(u8g2_font_ncenR08_tr);
  display.setCursor(100, 24);
  display.print(watch.getAmPmC(true));
  display.setFont(u8g2_font_helvB12_tf);
  String dateString = watch.getTime("%a %d %b");
  textWidth = display.getStrWidth(dateString.c_str());
  display.setCursor((SCREEN_WIDTH - textWidth) / 2, 43);
  display.print(dateString);
}
void updateDHTIfNeeded() {
  unsigned long now = millis();
  if (now - lastDHTUpdate > DHT_UPDATE_INTERVAL) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temp = t;
      humi = h;
    }
    lastDHTUpdate = now;
  }
}
void drawDHTData() {
  display.setDrawColor(0);
  display.drawBox(0, 64, SCREEN_WIDTH, 30);  // Xoá vùng nhiệt độ, độ ẩm
  display.setDrawColor(1);
  display.setFont(u8g2_font_helvB14_tf);
  if (!isnan(temp) && !isnan(humi)) {
    String dhtStr = String(temp, 1) + "°C  " + String(humi, 1) + "%";
    display.setCursor(10, 64);
    display.print(dhtStr);
  } else {
    display.setCursor(30, 64);
    display.print("DHT error");
  }
}
void printLocalTime() {
  display.clearBuffer();  
  drawTimeAndDate(); 
  drawDHTData();
  drawBluetoothAndBattery();
  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  display.begin();
  display.enableUTF8Print();
  dht.begin();
  watch.begin();
  watch.setConnectionCallback(connectionCallback);
  watch.setNotificationCallback(notificationCallback);
  watch.setConfigurationCallback(configCallback);
  

  Serial.println(watch.getAddress());
  watch.setBattery(80);
}

void loop() {
  watch.loop();
  updateDHTIfNeeded();

  // Ưu tiên navigation
  if (isNavActive && currentState != SHOW_NAVIGATION) {
    previousState = currentState;
    currentState = SHOW_NAVIGATION;
    change = true;  // Buộc vẽ lại
  }

  // Nếu có notification mới và không navigation thì hiển thị
  if (newNotification && !isNavActive && currentState != SHOW_NOTIFICATION) {
    previousState = currentState;  // Lưu trạng thái hiện tại
    currentState = SHOW_NOTIFICATION;
    change = true;  // Buộc vẽ lại
  }

  switch (currentState) {
    case SHOW_NAVIGATION:
      if (change) {
        change = false;
        Navigation nav = watch.getNavigation();

        // Xác định cần vẽ toàn bộ hay chỉ cập nhật
        bool needFull = false;
        if (!firstNavShown) {
          needFull = true;
        } else if (nav.icon != lastNav.icon || nav.title != lastNav.title) {
          needFull = true;
        }

        if (needFull) {
          displayNavigationFull(nav);
          firstNavShown = true;
        } else {
          displayNavigationPartial(nav);
        }

        lastNav = nav;  // Cập nhật giá trị để so sánh lần sau
      }

      // Nếu điều hướng kết thúc thì quay lại trạng thái trước
      if (!isNavActive) {
        currentState = previousState;
        firstNavShown = false;  // Để lần sau vào lại sẽ vẽ full
        if (currentState == SHOW_CLOCK) printLocalTime();
        else if (currentState == SHOW_WEATHER) display_Weather();
        else if (currentState == SHOW_DASAI) display_dasai();
        display.sendBuffer();
      }
      break;

    case SHOW_NOTIFICATION:
      if (!notificationDisplayed) {
          notificationTimestamp = millis();  // Đặt lại thời gian tại đây
          notificationDisplayed = true;
        }

        if (millis() - notificationTimestamp > (isIncomingCall ? 20000 : NOTIF_DISPLAY_DURATION)) {
          newNotification = false;
          isIncomingCall = false;
          notificationDisplayed = false;

        currentState = previousState;  // Trở lại trạng thái trước khi thông báo

          // Gọi lại màn hình trước đó
          if (currentState == SHOW_CLOCK) printLocalTime();
          else if (currentState == SHOW_WEATHER) display_Weather();
          else if (currentState == SHOW_DASAI) display_dasai();      
          display.sendBuffer();
        }
      break;

    case SHOW_CLOCK:
      static unsigned long lastUpdate = 0;
      if (millis() - lastUpdate > 1000) {
        printLocalTime();
        // dasai_Mochi();  // Thêm nền/animation nếu muốn
        lastUpdate = millis();
      }
      break;

    case SHOW_DASAI:
      display_dasai();      
      break;
    case SHOW_WEATHER:
      display_Weather();      
      break;  
  }

}

void display_dasai(){
  static unsigned long lastFrameTime = 0;
  static int currentFrame = 0;
  unsigned long now = millis();

  if (now - lastFrameTime >= FRAME_DELAY) {
    lastFrameTime = now;

    display.clearBuffer();                                            // xóa RAM buffer
    display.drawXBMP(0, 0, FRAME_WIDTH, FRAME_HEIGHT,                 // vẽ bitmap
                  frames[currentFrame]);
    display.sendBuffer();                                             // đẩy toàn bộ lên OLED

    currentFrame = (currentFrame + 1) % TOTAL_FRAMES;              // vòng lặp frame
  }
}
void display_Weather() {
  if (watch.getWeatherCount() > 0) {
    Weather w = watch.getWeatherAt(0);  // hôm nay

    String city = watch.getWeatherCity();
    // String icon = w.icon;
    int temp = w.temp;
    int high = w.high;
    int low = w.low;
    int uv = w.uv;
    int pressure = w.pressure;
    String description = getWeatherDescription(w.icon);
    String timeString = watch.getTime("%a %d %b") + "--" + watch.getHourZ() + watch.getTime(":%M:%S");
    display.clearBuffer();
    // Serial.println("Icon: " + w.icon);
    // Serial.println("High: " +w.high);
    // Serial.println("Temp: " +w.temp);
    // Serial.println("UV: " +w.uv);
    // Hiển thị icon thời tiết
    // drawWeatherIcon32(icon, 0, 0); // Hàm tự ánh xạ icon → bitmap
    const uint8_t* iconBitmap = getWeatherIconBitmap(w.icon);
    display.drawXBMP(-5, 0, 128, 64, iconBitmap);
    // // In thông tin
    display.setFont(u8g2_font_helvB08_tf);
    display.setCursor(0, 11);
    display.print(city); 
    display.printf(" T: %d°C", temp);
    // display.setCursor(0, 24);
    // display.printf("High: %d°C  Low: %d°C", high, low);

    display.setCursor(55, 24);
    display.print("UV: ");
    display.print(uv);   // In số lẻ
    display.printf(" H: %d°C", high);
    display.setCursor(55, 36);
    display.print("P:");
    display.print(pressure);
    display.print(" hPa");    
    display.setFont(u8g2_font_unifont_t_vietnamese1);
    // int iconWidth = display.getStrWidth(description.c_str());
    // display.setCursor((SCREEN_WIDTH - iconWidth) / 2, 52);
    display.setCursor(50, 52);
    display.print(description);  // Ví dụ: Nhiều mây
    display.setFont(u8g2_font_helvB08_tf);
    int textWidth = display.getStrWidth(timeString.c_str());
    display.setCursor((SCREEN_WIDTH - textWidth) / 2, 64);
    display.print(timeString);

    display.sendBuffer();

  }
}
String getWeatherDescription(int icon) {

  switch (icon) {
    case 1: return "Trời quang";
    case 2: return "Ít mây";
    case 3: return "Mây rải rác";
    case 4: return "Nhiều mây";
    case 5: return "Mưa nhẹ";
    case 6: return "Mưa to";
    case 7: return "Dông bão";
    default: return "Không rõ";
  }
}
const uint8_t* getWeatherIconBitmap(int icon) {

  switch (icon) {
    case 1: return epd_bitmap_01d;
    case 2: return epd_bitmap_02d;
    case 3: return epd_bitmap_03d;
    case 4: return epd_bitmap_04d;
    case 9: return epd_bitmap_09d;
    case 10: return epd_bitmap_10d;
    case 11: return epd_bitmap_11d;
    case 13: return epd_bitmap_13d;
    case 50: return epd_bitmap_50d;
    // default: return epd_bitmap_House;
  }
}
/*
void display_dasai() {
  for (uint8_t i = 0; i < FRAME_COUNT; i++) {
    display.clearBuffer();
    display.drawBitmap(0, 0, 128, 64, FRAME_ARRAY[i]);
    display.sendBuffer();
    delay(100); // Sesuaikan kecepatan animasi
  }  
}
*/