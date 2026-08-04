/*
  phase8_earthquake_detection.cpp
  -----------------------------------------------------------
  ห้องโถง: ตรวจแรงสั่นสะเทือนคล้ายแผ่นดินไหวจาก MPU-6050 (ยึดติดกับโครงบ้านจำลอง)
  ตรวจจับด้วยขนาดเวกเตอร์ความเร่งรวม (accel magnitude) เทียบกับค่าแรงโน้มถ่วงปกติ
  (~9.8 m/s^2) ถ้าเบี่ยงเบนเกินเกณฑ์ต่อเนื่องหลายตัวอย่างในช่วงเวลาสั้นๆ (ไม่ใช่แค่
  โดนกระแทกทีเดียว) ถือว่าเป็นการสั่นแบบแผ่นดินไหว

  Buzzer ดังทันทีจาก loop() ตรงๆ ไม่รอเครือข่าย (แพทเทิร์นเดียวกับ phase7 SOS)
  ค้างไว้จนกว่าจะกดยกเลิกจากเว็บ dashboard (ไม่ auto-stop เอง เพราะเป็นเหตุฉุกเฉิน)

  ขา: ดู include/hall_pins.h (MPU_SDA_PIN=21, MPU_SCL_PIN=47 ยืนยันแล้วใน Phase 2,
  EARTHQUAKE_BUZZER_PIN=2 ใช้ร่วมกับ phase5d/phase7)
  ต้องมี include/wifi_credentials.h ก่อน (เหมือนเฟสอื่นๆ)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "hall_pins.h"
#include "wifi_credentials.h"

// ----- เกณฑ์ตรวจจับ (ปรับได้ตามผลทดสอบจริง — อาจต้องลอง/ปรับหลังทดสอบเขย่าจริง) -----
const float GRAVITY_MS2 = 9.8f;
const float SHAKE_THRESHOLD_MS2 = 3.0f;  // ค่าเบี่ยงเบนจาก 1g ที่ถือว่า "สั่น" ในตัวอย่างนั้น
const int WINDOW_SIZE = 20;              // จำนวนตัวอย่างล่าสุดที่ดูย้อนหลัง (~1 วิ ที่ 50ms/ตัวอย่าง)
const int WINDOW_TRIGGER_COUNT = 8;      // ต้องสั่นอย่างน้อยกี่ตัวอย่างใน window ถึงจะถือว่าเป็นแผ่นดินไหวจริง
const unsigned long SAMPLE_INTERVAL_MS = 50;

Adafruit_MPU6050 mpu;
bool mpuFound = false;

bool shakeWindow[WINDOW_SIZE] = {false};
int windowIndex = 0;
int shakeCountInWindow = 0;

httpd_handle_t webHttpd = NULL;
SemaphoreHandle_t stateMutex;

volatile bool eqActive = false;
unsigned long eqTimestampMs = 0;
float eqPeakDelta = 0;

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="th">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ห้องโถง - ตรวจแผ่นดินไหว</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 0; padding: 32px 16px; transition: background 0.3s; }
    body.ok { background: #111; color: #eee; }
    body.alert { background: #b71c1c; color: #fff; }
    #big { font-size: 32px; font-weight: bold; margin-top: 20px; }
    #detail { margin-top: 10px; font-size: 16px; opacity: 0.8; }
    button {
      margin-top: 28px; padding: 18px 32px; font-size: 18px; border: none;
      border-radius: 8px; background: #fff; color: #b71c1c; font-weight: bold; cursor: pointer;
    }
  </style>
</head>
<body id="body" class="ok">
  <h2>ห้องโถง — ตรวจแรงสั่นสะเทือน</h2>
  <div id="big">ปกติ ไม่มีการสั่นผิดปกติ</div>
  <div id="detail"></div>
  <button id="cancelBtn" style="display:none" onclick="cancelAlert()">รับทราบ / ยกเลิกการแจ้งเตือน</button>
  <script>
    function refresh() {
      fetch('/status').then(r => r.json()).then(s => {
        const body = document.getElementById('body');
        const big = document.getElementById('big');
        const detail = document.getElementById('detail');
        const btn = document.getElementById('cancelBtn');
        if (s.active) {
          body.className = 'alert';
          big.innerText = '🚨 ตรวจพบการสั่นสะเทือนคล้ายแผ่นดินไหว!';
          detail.innerText = 'ผ่านไปแล้ว ' + Math.floor(s.sinceMs / 1000) + ' วินาที | ค่าเบี่ยงเบนสูงสุด ' + s.peakDelta.toFixed(2) + ' m/s²';
          btn.style.display = 'inline-block';
        } else {
          body.className = 'ok';
          big.innerText = 'ปกติ ไม่มีการสั่นผิดปกติ';
          detail.innerText = s.mpuOk ? '' : '(ไม่พบเซนเซอร์ MPU-6050)';
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
  float peakDelta;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  active = eqActive;
  sinceMs = active ? (millis() - eqTimestampMs) : 0;
  peakDelta = eqPeakDelta;
  xSemaphoreGive(stateMutex);

  char json[128];
  snprintf(json, sizeof(json), "{\"active\":%s,\"sinceMs\":%lu,\"peakDelta\":%.2f,\"mpuOk\":%s}",
           active ? "true" : "false", sinceMs, peakDelta, mpuFound ? "true" : "false");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t cancelHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  eqActive = false;
  eqPeakDelta = 0;
  digitalWrite(EARTHQUAKE_BUZZER_PIN, LOW);
  xSemaphoreGive(stateMutex);

  // เคลียร์ window กันสั่นตกค้างจากช่วงที่แจ้งเตือนอยู่ทำให้ trigger ซ้ำทันที
  for (int i = 0; i < WINDOW_SIZE; i++) shakeWindow[i] = false;
  shakeCountInWindow = 0;

  Serial.println(">>> ยกเลิกการแจ้งเตือนแผ่นดินไหวจากเว็บแล้ว <<<");

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
  Serial.println("===== ห้องโถง: ตรวจแรงสั่นสะเทือนแผ่นดินไหว (MPU-6050) =====");

  stateMutex = xSemaphoreCreateMutex();

  pinMode(EARTHQUAKE_BUZZER_PIN, OUTPUT);
  digitalWrite(EARTHQUAKE_BUZZER_PIN, LOW);

  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
  Wire.setClock(100000);  // บังคับ I2C standard-mode 100kHz กันอ่านพัง (เจอ i2cWriteReadNonStop Error -1 ถี่ตอนใช้ค่า default)
  if (!mpu.begin()) {
    Serial.println("!!! MPU-6050 INIT FAILED — เช็คสาย SDA/SCL และไฟเลี้ยง !!!");
  } else {
    Serial.println(">>> MPU-6050 INIT OK <<<");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpuFound = true;
  }

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
    Serial.println("!!! WiFi ต่อไม่ติด — การตรวจจับ+buzzer ยังทำงานได้ปกติ (ไม่พึ่งเครือข่าย) !!!");
  }

  Serial.println("เริ่มตรวจแรงสั่นสะเทือน...");
}

void loop() {
  static unsigned long lastSampleMs = 0;

  if (!mpuFound || millis() - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSampleMs = millis();

  sensors_event_t a, g, temp;
  bool readOk = mpu.getEvent(&a, &g, &temp);

  float magnitude = sqrtf(a.acceleration.x * a.acceleration.x +
                           a.acceleration.y * a.acceleration.y +
                           a.acceleration.z * a.acceleration.z);

  // ถ้าอ่าน I2C พัง (สายหลวม/บัสรบกวน) ค่าที่ได้มักเป็น 0 หรือใกล้ 0 ทั้งหมด
  // (ผิดธรรมชาติมาก เพราะแม้วางนิ่งก็ควรได้ ~9.8 จากแรงโน้มถ่วงเสมอ) ต้องทิ้งตัวอย่าง
  // นี้ไปเลย ไม่งั้นจะเข้าใจผิดว่าความเร่งหายวับไปทันที กลายเป็นแจ้งเตือนเท็จ
  if (!readOk || magnitude < 1.0f) {
    return;
  }

  float delta = fabsf(magnitude - GRAVITY_MS2);
  bool shaking = delta > SHAKE_THRESHOLD_MS2;

  // sliding window แบบ circular buffer นับจำนวนตัวอย่างที่ "สั่น" ใน N ตัวอย่างล่าสุด
  if (shakeWindow[windowIndex] != shaking) {
    shakeCountInWindow += shaking ? 1 : -1;
    shakeWindow[windowIndex] = shaking;
  }
  windowIndex = (windowIndex + 1) % WINDOW_SIZE;

  bool wasActive;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  wasActive = eqActive;
  if (shaking && delta > eqPeakDelta) eqPeakDelta = delta;
  xSemaphoreGive(stateMutex);

  if (!wasActive && shakeCountInWindow >= WINDOW_TRIGGER_COUNT) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    eqActive = true;
    eqTimestampMs = millis();
    digitalWrite(EARTHQUAKE_BUZZER_PIN, HIGH);
    xSemaphoreGive(stateMutex);
    Serial.printf(">>> ตรวจพบแรงสั่นสะเทือนคล้ายแผ่นดินไหว! (เบี่ยงเบนล่าสุด %.2f m/s^2) Buzzer ดังทันที <<<\n", delta);
  }
}
