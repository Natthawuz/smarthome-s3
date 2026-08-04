/*
  phase9_kitchen.cpp
  -----------------------------------------------------------
  ห้องครัว: ตรวจ CO (คาร์บอนมอนอกไซด์) ด้วย MQ-7 — ถ้าค่าเกินเกณฑ์ต่อเนื่องนานเกิน
  เวลาที่ตั้งไว้ (กันแจ้งเตือนเท็จจากควันทำอาหารที่มาแล้วหายไปเร็ว) ถือว่าเป็นเหตุจริง
  → สั่งปั๊มน้ำทำงานผ่านรีเลย์เป็นช่วงสั้นๆ (ฉีดน้ำดับไฟเบื้องต้น) + buzzer ดังทันที
  ค้างไว้จนกว่าจะกดรับทราบจากเว็บ (แพทเทิร์นเดียวกับ phase7/phase8)

  หมายเหตุ: MQ-7 อ่านได้แค่ค่าดิบจาก ADC (0-4095) ไม่ได้แปลงเป็น ppm จริง เพราะการ
  คาลิเบรตที่แม่นยำต้องมีขั้นตอน burn-in/ปรับ load resistor ซับซ้อนเกินความจำเป็น
  ของโปรเจกต์นี้ — ใช้ threshold ค่าดิบแทน (ต้องทดสอบกับควัน/แก๊สจริงแล้วปรับเกณฑ์)

  ขา: ดู include/kitchen_pins.h (MQ7=IO1, Relay=IO39, Buzzer=IO2 — ไม่มีขาไหนยืม
  จากเฟสอื่นเลย หลังจากถอด SD card และย้าย DHT22 ห้องนอนไป IO38 แล้ว)
  ต้องมี include/wifi_credentials.h ก่อน (เหมือนเฟสอื่นๆ)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "kitchen_pins.h"
#include "wifi_credentials.h"

// ----- เกณฑ์ตรวจจับ (ค่าเริ่มต้น ต้องทดสอบ/ปรับกับควัน-แก๊สจริง) -----
// วัด baseline จริงตอนอากาศปกติได้ ~2540-2611 หลังวอร์มอัพ ~10 นาที (2026-07-31)
// ตั้งเกณฑ์เผื่อระยะห่างจาก baseline พอสมควร กันค่าแกว่ง/ดริฟท์เล็กน้อยหลอกจับผิด
// ถ้าใช้งานจริงแล้วเจอ false alarm บ่อย/ไวไม่พอ ให้ปรับตัวเลขนี้ตามผลจริง
const int CO_THRESHOLD_RAW = 2000;             // ค่าดิบ ADC (0-4095) ที่ถือว่า CO เริ่มสูงผิดปกติ
const unsigned long SUSTAINED_DURATION_MS = 3000;  // ต้องเกินเกณฑ์ต่อเนื่องกี่ ms ถึงจะถือว่าเป็นเหตุจริง (กันควันทำอาหาร)
const unsigned long PUMP_BURST_MS = 3000;      // ปั๊มน้ำทำงานกี่ ms ต่อการแจ้งเตือนหนึ่งครั้ง (ฉีดเป็นช่วง ไม่ใช่ค้างตลอด)
const unsigned long SAMPLE_INTERVAL_MS = 200;

httpd_handle_t webHttpd = NULL;
SemaphoreHandle_t stateMutex;

volatile bool alertActive = false;
unsigned long alertTimestampMs = 0;
int latestCoRaw = 0;
bool pumpOn = false;
unsigned long pumpStartMs = 0;
bool relayManualOn = false;  // ปุ่มทดสอบ relay มือ แยกจาก pumpOn อัตโนมัติ ไว้เช็ค LED/สาย

unsigned long aboveThresholdSinceMs = 0;  // 0 = ยังไม่เกินเกณฑ์อยู่

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="th">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ห้องครัว - ตรวจ CO</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 0; padding: 32px 16px; transition: background 0.3s; }
    body.ok { background: #111; color: #eee; }
    body.alert { background: #b71c1c; color: #fff; }
    #big { font-size: 30px; font-weight: bold; margin-top: 20px; }
    #detail { margin-top: 10px; font-size: 16px; opacity: 0.8; }
    #co { margin-top: 16px; font-size: 15px; opacity: 0.7; }
    button {
      margin-top: 28px; padding: 18px 32px; font-size: 18px; border: none;
      border-radius: 8px; background: #fff; color: #b71c1c; font-weight: bold; cursor: pointer;
    }
  </style>
</head>
<body id="body" class="ok">
  <h2>ห้องครัว — ตรวจ CO</h2>
  <div id="big">ปกติ ไม่มีการแจ้งเตือน</div>
  <div id="detail"></div>
  <div id="co"></div>
  <button id="cancelBtn" style="display:none" onclick="cancelAlert()">รับทราบ / ยกเลิกการแจ้งเตือน</button>
  <button id="relayBtn" onclick="toggleRelay()">ทดสอบ Relay (มือ)</button>
  <script>
    function refresh() {
      fetch('/status').then(r => r.json()).then(s => {
        const body = document.getElementById('body');
        const big = document.getElementById('big');
        const detail = document.getElementById('detail');
        const btn = document.getElementById('cancelBtn');
        document.getElementById('co').innerText = 'ค่า CO ดิบ: ' + s.coRaw + (s.pumpOn ? ' | ปั๊มน้ำ: กำลังฉีดน้ำ' : '');
        if (s.active) {
          body.className = 'alert';
          big.innerText = '🚨 ตรวจพบ CO สูงผิดปกติต่อเนื่อง!';
          detail.innerText = 'ผ่านไปแล้ว ' + Math.floor(s.sinceMs / 1000) + ' วินาที';
          btn.style.display = 'inline-block';
        } else {
          body.className = 'ok';
          big.innerText = 'ปกติ ไม่มีการแจ้งเตือน';
          detail.innerText = '';
          btn.style.display = 'none';
        }
        const relayBtn = document.getElementById('relayBtn');
        relayBtn.dataset.on = s.relayManualOn ? '1' : '0';
        relayBtn.innerText = s.relayManualOn ? 'ปิด Relay (มือ)' : 'ทดสอบ Relay (มือ)';
      }).catch(() => {});
    }
    function cancelAlert() {
      fetch('/cancel').then(refresh);
    }
    function toggleRelay() {
      const btn = document.getElementById('relayBtn');
      const turningOn = btn.dataset.on !== '1';
      btn.dataset.on = turningOn ? '1' : '0';
      fetch(turningOn ? '/relay-on' : '/relay-off').then(refresh);
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
  bool active, pump;
  unsigned long sinceMs;
  int coRaw;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  active = alertActive;
  sinceMs = active ? (millis() - alertTimestampMs) : 0;
  coRaw = latestCoRaw;
  pump = pumpOn;
  xSemaphoreGive(stateMutex);

  char json[160];
  snprintf(json, sizeof(json), "{\"active\":%s,\"sinceMs\":%lu,\"coRaw\":%d,\"pumpOn\":%s,\"relayManualOn\":%s}",
           active ? "true" : "false", sinceMs, coRaw, pump ? "true" : "false", relayManualOn ? "true" : "false");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t cancelHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  alertActive = false;
  digitalWrite(KITCHEN_BUZZER_PIN, LOW);
  xSemaphoreGive(stateMutex);

  aboveThresholdSinceMs = 0;  // เริ่มนับใหม่ กันสั่นตกค้างทำให้ trigger ซ้ำทันที

  Serial.println(">>> ยกเลิกการแจ้งเตือน CO จากเว็บแล้ว <<<");

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ปุ่มทดสอบ relay มือ — สั่งขา RELAY_PIN ตรงๆ แยกจาก logic อัตโนมัติของ CO
// ไว้เช็คว่า relay ทำงานจริงไหม (ไฟ LED ติด/มีเสียงคลิก) เวลาแก้ปัญหาไฟ/สาย/polarity
static esp_err_t relayOnHandler(httpd_req_t *req) {
  relayManualOn = true;
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println(">>> ทดสอบ relay: สั่ง HIGH จากเว็บ <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t relayOffHandler(httpd_req_t *req) {
  relayManualOn = false;
  digitalWrite(RELAY_PIN, LOW);
  Serial.println(">>> ทดสอบ relay: สั่ง LOW จากเว็บ <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};
  httpd_uri_t cancelUri = {"/cancel", HTTP_GET, cancelHandler, NULL};
  httpd_uri_t relayOnUri = {"/relay-on", HTTP_GET, relayOnHandler, NULL};
  httpd_uri_t relayOffUri = {"/relay-off", HTTP_GET, relayOffHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &statusUri);
    httpd_register_uri_handler(webHttpd, &cancelUri);
    httpd_register_uri_handler(webHttpd, &relayOnUri);
    httpd_register_uri_handler(webHttpd, &relayOffUri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ห้องครัว: ตรวจ CO (MQ-7) + ปั๊มน้ำ =====");

  stateMutex = xSemaphoreCreateMutex();

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(KITCHEN_BUZZER_PIN, OUTPUT);
  digitalWrite(KITCHEN_BUZZER_PIN, LOW);

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
    Serial.println("!!! WiFi ต่อไม่ติด — การตรวจจับ+buzzer+ปั๊มน้ำยังทำงานได้ปกติ (ไม่พึ่งเครือข่าย) !!!");
  }

  Serial.println("เริ่มตรวจวัด CO...");
}

void loop() {
  static unsigned long lastSampleMs = 0;
  unsigned long now = millis();

  // ปิดปั๊มน้ำอัตโนมัติเมื่อครบเวลาฉีด (ไม่บล็อก loop ด้วย delay)
  if (pumpOn && now - pumpStartMs >= PUMP_BURST_MS) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    pumpOn = false;
    digitalWrite(RELAY_PIN, LOW);
    xSemaphoreGive(stateMutex);
    Serial.println(">>> ปั๊มน้ำหยุดทำงาน (ครบเวลาฉีด) <<<");
  }

  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSampleMs = now;

  int coRaw = analogRead(MQ7_PIN);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  latestCoRaw = coRaw;
  bool wasActive = alertActive;
  xSemaphoreGive(stateMutex);

  bool overThreshold = coRaw > CO_THRESHOLD_RAW;

  if (overThreshold) {
    if (aboveThresholdSinceMs == 0) {
      aboveThresholdSinceMs = now;  // เพิ่งเริ่มเกินเกณฑ์
    } else if (!wasActive && now - aboveThresholdSinceMs >= SUSTAINED_DURATION_MS) {
      // เกินเกณฑ์ต่อเนื่องนานพอแล้ว ถือว่าเป็นเหตุจริง ไม่ใช่ควันทำอาหารผ่านๆ
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      alertActive = true;
      alertTimestampMs = now;
      pumpOn = true;
      pumpStartMs = now;
      digitalWrite(RELAY_PIN, HIGH);
      digitalWrite(KITCHEN_BUZZER_PIN, HIGH);
      xSemaphoreGive(stateMutex);
      Serial.printf(">>> ตรวจพบ CO สูงต่อเนื่อง! (ค่าดิบ %d) สั่งปั๊มน้ำ + Buzzer ดังทันที <<<\n", coRaw);
    }
  } else {
    aboveThresholdSinceMs = 0;  // ลดลงต่ำกว่าเกณฑ์แล้ว รีเซ็ตตัวจับเวลา
  }

  static unsigned long lastDebugPrint = 0;
  if (now - lastDebugPrint > 1000) {
    Serial.printf("[debug] coRaw=%d\n", coRaw);
    lastDebugPrint = now;
  }
}
