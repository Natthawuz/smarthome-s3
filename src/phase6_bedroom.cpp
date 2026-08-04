/*
  phase6_bedroom.cpp
  -----------------------------------------------------------
  ห้องนอน: วัดอุณหภูมิ+ความชื้นด้วย DHT22 + แสดงผลบนจอ OLED 1.3" ในห้อง +
  ส่งขึ้นเว็บ dashboard แบบเรียลไทม์ (รันเว็บในตัวบอร์ดเอง แพทเทิร์นเดียวกับ
  phase3/4/5b/5d — ไม่ใช้ MQTT/Node-RED ตามที่ตัดสินใจแล้ว)

  หมายเหตุ: เอกสารโครงการเดิมระบุ DS18B20 (วัดอุณหภูมิอย่างเดียว) แต่ฮาร์ดแวร์จริง
  ที่มีคือ DHT22 (AM2302) ซึ่งวัดความชื้นได้ด้วยในตัว จึงแสดงทั้งคู่

  หมายเหตุ (แก้ครั้งที่ 3): จอ 1.3" ที่มีจริงใช้ชิปควบคุม SH1106 (ยืนยันจากผู้ใช้)
  ไม่ใช่ SSD1306 — ตอนแรกใช้ไลบรารี Adafruit_SSD1306 ทำให้จอแสดงไม่เต็ม (SH1106 มี
  RAM ภายใน 132x64 ต่างจาก SSD1306 ที่เป็น 128x64 พอดี) จึงเปลี่ยนมาใช้
  Adafruit_SH110X (คลาส Adafruit_SH1106G) แทน

  ขา: ดู include/bedroom_pins.h
  ต้องมี include/wifi_credentials.h ก่อน (เหมือนเฟสอื่นๆ)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "bedroom_pins.h"
#include "wifi_credentials.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_ADDR   0x3C  // ที่อยู่ I2C ปกติของ SH1106 ส่วนใหญ่ (ถ้าจอไม่ขึ้นให้ลอง 0x3D)

DHT dht(DHT22_PIN, DHT22);
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

httpd_handle_t webHttpd = NULL;
float latestTempC = NAN;
float latestHumidity = NAN;

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="th">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ห้องนอน - อุณหภูมิ/ความชื้น</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; margin: 0; padding: 32px 16px; }
    #temp { font-size: 64px; font-weight: bold; margin-top: 20px; }
    #humidity { font-size: 28px; margin-top: 8px; color: #90caf9; }
    #status { margin-top: 12px; font-size: 14px; color: #9e9e9e; }
  </style>
</head>
<body>
  <h2>ห้องนอน</h2>
  <div id="temp">--°C</div>
  <div id="humidity">ความชื้น --%</div>
  <div id="status">กำลังโหลด...</div>
  <script>
    function refresh() {
      fetch('/status').then(r => r.json()).then(s => {
        document.getElementById('temp').innerText =
          s.ok ? s.tempC.toFixed(1) + '°C' : '- -°C';
        document.getElementById('humidity').innerText =
          s.ok ? 'ความชื้น ' + s.humidity.toFixed(0) + '%' : 'ความชื้น --%';
        document.getElementById('status').innerText =
          s.ok ? 'อัปเดตล่าสุด: เมื่อสักครู่' : 'ไม่พบเซนเซอร์ DHT22';
      }).catch(() => {
        document.getElementById('status').innerText = 'เชื่อมต่อไม่ได้';
      });
    }
    setInterval(refresh, 2000);
    refresh();
  </script>
</body>
</html>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t statusHandler(httpd_req_t *req) {
  char json[128];
  if (!isnan(latestTempC) && !isnan(latestHumidity)) {
    snprintf(json, sizeof(json), "{\"ok\":true,\"tempC\":%.2f,\"humidity\":%.2f}",
             latestTempC, latestHumidity);
  } else {
    snprintf(json, sizeof(json), "{\"ok\":false}");
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &statusUri);
  }
}

void updateDisplay(float tempC, float humidity, bool ok) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("หองนอน");
  display.drawLine(0, 10, OLED_WIDTH - 1, 10, SH110X_WHITE);

  display.setTextSize(3);
  display.setCursor(10, 18);
  if (ok) {
    display.printf("%.1fC", tempC);
  } else {
    display.println("--.-C");
  }

  display.setTextSize(1);
  display.setCursor(10, 50);
  if (ok) {
    display.printf("Humidity: %.0f%%", humidity);
  } else {
    display.println("no sensor");
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ห้องนอน: DHT22 + OLED + เว็บ dashboard =====");

  dht.begin();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("!!! OLED INIT FAILED (เช็คสาย SDA/SCL หรือลองที่อยู่ 0x3D) !!!");
  } else {
    Serial.println(">>> OLED OK <<<");
    display.clearDisplay();
    display.display();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("กำลังเชื่อมต่อ WiFi \"%s\"", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print(">>> WiFi OK เปิดเบราว์เซอร์ไปที่: http://");
  Serial.println(WiFi.localIP());

  startWebServer();
  Serial.println("เริ่มวัดอุณหภูมิ/ความชื้น...");
}

void loop() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("อ่านค่า DHT22 ไม่สำเร็จ (สายหลุด/รบกวนสัญญาณ?)");
    updateDisplay(0, 0, false);
  } else {
    latestTempC = t;
    latestHumidity = h;
    Serial.printf("อุณหภูมิห้องนอน: %.2f°C, ความชื้น: %.2f%%\n", t, h);
    updateDisplay(t, h, true);
  }

  delay(2000);  // DHT22 อ่านได้เร็วสุดทุก ~2 วิ (ข้อจำกัดของเซนเซอร์เอง)
}
