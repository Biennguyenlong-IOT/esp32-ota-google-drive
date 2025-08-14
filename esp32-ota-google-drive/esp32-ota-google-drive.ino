#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>  // Thư viện WiFiManager

const char* device_id = "Mini_Oled_Navigation";
const char* current_version = "v0.0";
const char* version_json_url = "https://biennguyenlong-iot.github.io/esp32-ota-google-drive/versions.json";

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Khởi tạo WiFiManager
  WiFiManager wm;

  // Tên AP sẽ hiện khi không kết nối được WiFi
  bool res = wm.autoConnect("ESP32-OTA-Setup");

  if (!res) {
    Serial.println("❌ Failed to connect, restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("✅ Connected to WiFi");

  // Kiểm tra OTA sau khi kết nối
  checkForUpdate();
}

void loop() {
  delay(60000);  // Kiểm tra mỗi phút
  checkForUpdate();
}

void checkForUpdate() {
  WiFiClientSecure client;
  client.setInsecure();  // ⚠️ Chấp nhận mọi chứng chỉ SSL (dành cho GitHub Pages)

  HTTPClient http;
  http.begin(client, version_json_url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (err) {
      Serial.println("❌ Failed to parse JSON");
      return;
    }

    String latest_version = doc[device_id]["version"];
    String bin_url = doc[device_id]["bin"];

    Serial.println("Current version: " + String(current_version));
    Serial.println("Latest version: " + latest_version);

    if (latest_version != current_version) {
      Serial.println("⬇️ New version available: " + latest_version);
      performOTA(bin_url);
    } else {
      Serial.println("✅ Firmware is up to date");
    }
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
  }
  http.end();
}

void performOTA(const String& bin_url) {
  WiFiClientSecure client;
  client.setInsecure();  // ⚠️ Không an toàn cho sản phẩm thương mại

  HTTPClient http;
  http.begin(client, bin_url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    int contentLength = http.getSize();
    bool canBegin = Update.begin(contentLength);
    if (canBegin) {
      WiFiClient* stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);

      if (Update.end()) {
        if (Update.isFinished()) {
          Serial.println("✅ Update successful. Rebooting...");
          delay(1000);
          ESP.restart();
        } else {
          Serial.println("❌ Update not finished.");
        }
      } else {
        Serial.printf("❌ Update error: %s\n", Update.errorString());
      }
    } else {
      Serial.println("❌ Not enough space for OTA");
    }
  } else {
    Serial.println("❌ HTTP error code: " + String(httpCode));
  }
  http.end();
}
