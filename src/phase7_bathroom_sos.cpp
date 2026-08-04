/*
  phase7_bathroom_sos.cpp
  -----------------------------------------------------------
  ห้องน้ำ: ปุ่มกดฉุกเฉิน (SOS) — กดแล้ว buzzer ดังทันทีโดยไม่รอเครือข่าย/WiFi เลย
  (เช็คปุ่มและสั่ง buzzer ใน loop() ตรงๆ ไม่พึ่งเว็บ/WiFi) พร้อมเว็บ dashboard
  แสดงสถานะการแจ้งเตือนแบบเรียลไทม์ และมีปุ่มยกเลิกการแจ้งเตือนจากเว็บ

  หมายเหตุ: เอกสารโครงการเดิมระบุให้แจ้งเตือนผ่าน Telegram Bot ด้วย แต่ผู้ใช้ตัดสินใจ
  ตัดออกแล้วใช้ web dashboard แจ้งเตือนแทน (ตามแพทเทิร์นเดียวกับเฟสอื่นๆ ที่ไม่ใช้
  บริการภายนอก)

  buzzer ดังค้างไว้จนกว่าจะกดยกเลิกจากเว็บ (ไม่ auto-stop เอง เพราะเป็นเหตุฉุกเฉิน
  ต้องมีคนมายืนยันว่ารับทราบแล้วจริงๆ)

  ขา: ดู include/bathroom_pins.h
  ต้องมี include/wifi_credentials.h ก่อน (เหมือนเฟสอื่นๆ)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "bathroom_pins.h"
#include "wifi_credentials.h"

httpd_handle_t webHttpd = NULL;
SemaphoreHandle_t stateMutex;

volatile bool sosActive = false;
unsigned long sosTimestampMs = 0;

// โมดูลปุ่ม SOS (มี GND/VCC/S ในตัว) ไม่รู้ล่วงหน้าว่ากดแล้วเป็น HIGH หรือ LOW
// (แล้วแต่ยี่ห้อ) จึงจับค่า "ตอนไม่กด" ไว้ตอนบูตเป็น baseline แล้วดูว่าเปลี่ยนจากนั้น
// แทน ใช้ได้ทั้งโมดูล active-high และ active-low โดยไม่ต้องรู้สเปกล่วงหน้า
int buttonIdleState = HIGH;

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="th">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ห้องน้ำ - ปุ่ม SOS</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 0; padding: 32px 16px; transition: background 0.3s; }
    body.ok { background: #111; color: #eee; }
    body.alert { background: #b71c1c; color: #fff; }
    #big { font-size: 40px; font-weight: bold; margin-top: 20px; }
    #time { margin-top: 10px; font-size: 16px; opacity: 0.8; }
    button {
      margin-top: 28px; padding: 18px 32px; font-size: 18px; border: none;
      border-radius: 8px; background: #fff; color: #b71c1c; font-weight: bold; cursor: pointer;
    }
  </style>
</head>
<body id="body" class="ok">
  <h2>ห้องน้ำ — ปุ่ม SOS</h2>
  <div id="big">ปกติ ไม่มีการแจ้งเตือน</div>
  <div id="time"></div>
  <button id="cancelBtn" style="display:none" onclick="cancelAlert()">รับทราบ / ยกเลิกการแจ้งเตือน</button>
  <script>
    function refresh() {
      fetch('/status').then(r => r.json()).then(s => {
        const body = document.getElementById('body');
        const big = document.getElementById('big');
        const time = document.getElementById('time');
        const btn = document.getElementById('cancelBtn');
        if (s.active) {
          body.className = 'alert';
          big.innerText = '🚨 มีคนกดปุ่ม SOS ในห้องน้ำ!';
          time.innerText = 'ผ่านไปแล้ว ' + Math.floor(s.sinceMs / 1000) + ' วินาที';
          btn.style.display = 'inline-block';
        } else {
          body.className = 'ok';
          big.innerText = 'ปกติ ไม่มีการแจ้งเตือน';
          time.innerText = '';
          btn.style.display = 'none';
        }
      }).catch(() => {});
    }
    function cancelAlert() {
      fetch('/cancel').then(refresh);
    }
    setInterval(refresh, 1000);
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
  bool active;
  unsigned long sinceMs;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  active = sosActive;
  sinceMs = active ? (millis() - sosTimestampMs) : 0;
  xSemaphoreGive(stateMutex);

  char json[64];
  snprintf(json, sizeof(json), "{\"active\":%s,\"sinceMs\":%lu}",
           active ? "true" : "false", sinceMs);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t cancelHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sosActive = false;
  digitalWrite(BUZZER_PIN, LOW);
  xSemaphoreGive(stateMutex);

  Serial.println(">>> ยกเลิกการแจ้งเตือน SOS จากเว็บแล้ว <<<");

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};
  httpd_uri_t cancelUri = {"/cancel", HTTP_GET, cancelHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &statusUri);
    httpd_register_uri_handler(webHttpd, &cancelUri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ห้องน้ำ: ปุ่ม SOS =====");

  stateMutex = xSemaphoreCreateMutex();

  pinMode(SOS_BUTTON_PIN, INPUT);  // โมดูลมีวงจร pull-up/down ของตัวเองแล้ว ไม่ใช้ INPUT_PULLUP
  delay(100);
  buttonIdleState = digitalRead(SOS_BUTTON_PIN);
  Serial.printf("สถานะปุ่ม SOS ตอนไม่กด (baseline): %s\n", buttonIdleState == HIGH ? "HIGH" : "LOW");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // เชื่อม WiFi ก่อนเพื่อให้เว็บ dashboard ใช้ได้ แต่ปุ่ม SOS + buzzer ทำงานได้เสมอ
  // ไม่ว่า WiFi จะต่อติดหรือไม่ (เช็คใน loop() ตรงๆ ไม่รอผลตรงนี้)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("กำลังเชื่อมต่อ WiFi \"%s\"", WIFI_SSID);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(">>> WiFi OK เปิดเบราว์เซอร์ไปที่: http://");
    Serial.println(WiFi.localIP());
    startWebServer();
  } else {
    Serial.println("!!! WiFi ต่อไม่ติด — ปุ่ม SOS + buzzer ยังทำงานได้ปกติ (ไม่พึ่งเครือข่าย) !!!");
  }

  Serial.println("พร้อมรับการกดปุ่ม SOS...");
}

void loop() {
  // debounce มาตรฐาน: แยก "ค่าดิบล่าสุด" (ไว้จับจังหวะเริ่มนิ่ง) ออกจาก
  // "สถานะที่ยืนยันแล้ว" (ไว้เทียบว่าเปลี่ยนจริงหรือยัง) เดิมใช้ตัวแปรเดียวปนกัน
  // ทำให้พลาดจังหวะกด — อัปเดต lastRawPressed ก่อนถึงจะเช็ค trigger ทำให้
  // !lastRawPressed เป็น false ไปแล้วตอนถึงเวลาต้องยืนยัน
  static bool lastRawPressed = false;
  static bool confirmedPressed = false;
  static unsigned long lastDebounceMs = 0;

  int rawState = digitalRead(SOS_BUTTON_PIN);
  bool rawPressed = (rawState != buttonIdleState);  // ต่างจาก baseline = กำลังกดอยู่

  if (rawPressed != lastRawPressed) {
    lastDebounceMs = millis();
  }

  if (millis() - lastDebounceMs > 50 && rawPressed != confirmedPressed) {
    confirmedPressed = rawPressed;
    if (confirmedPressed) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      sosActive = true;
      sosTimestampMs = millis();
      digitalWrite(BUZZER_PIN, HIGH);
      xSemaphoreGive(stateMutex);
      Serial.println(">>> กดปุ่ม SOS! Buzzer ดังทันที <<<");
    }
  }

  lastRawPressed = rawPressed;

  delay(10);
}
