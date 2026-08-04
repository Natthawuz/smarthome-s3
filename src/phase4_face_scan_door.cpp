/*
  phase4_face_scan_door.cpp
  -----------------------------------------------------------
  รวมระบบห้องโถงเข้าเป็นไฟล์เดียว:
  - แสกนใบหน้าอัตโนมัติตลอดเวลา (ไม่ต้องกดปุ่ม/พิมพ์ตอบใน Serial) จำได้ -> servo ปลดล็อกเอง
  - เว็บดูภาพสด (ความละเอียด 240x240 ตามข้อจำกัดของโหมดจดจำใบหน้า) ที่ /stream (port 81)
  - ปุ่ม "เปิดประตู" บนเว็บไว้เป็นทางสำรอง ใช้ตอนแสกนหน้าไม่ผ่าน/ไม่มีกล้องมองเห็นหน้า

  ก่อนใช้ไฟล์นี้ ต้อง enroll ใบหน้าไว้ก่อนแล้ว (ใช้ environment phase2-face-recognition
  ทำครั้งเดียว ข้อมูลใบหน้าเก็บใน NVS ของบอร์ด ไม่ได้อยู่ในตัวไฟล์เฟิร์มแวร์ จึงยังอยู่ครบ
  แม้จะสลับมาอัปโหลดไฟล์นี้แทน) ถ้าต้อง enroll คนเพิ่ม ให้สลับกลับไปที่ environment
  phase2-face-recognition ชั่วคราว แล้วค่อยกลับมาที่ไฟล์นี้

  ต้องมีไฟล์ include/wifi_credentials.h ก่อน (เหมือน phase3)

  หมายเหตุการแก้บั๊ก: เดิม loop() (แสกนหน้า) กับ streamHandler() (ส่งภาพขึ้นเว็บ)
  ต่างเรียก esp_camera_fb_get() แย่งกล้องตัวเดียวกันพร้อมกันคนละ task ทำให้พอเปิดเว็บ
  ดูภาพสดค้างไว้ การแสกนหน้าเพี้ยน/ไม่ติด แก้โดยใช้ cameraMutex คุมให้ใช้กล้องได้ทีละ task
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ESP32Servo.h>
#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>
#include "hall_pins.h"
#include "wifi_credentials.h"

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

const int LOCK_ANGLE = 0;
const int UNLOCK_ANGLE = 90;
const unsigned long DOOR_OPEN_MS = 4000;
// กันแสกนซ้ำถี่เกินไปตอนหน้ายังอยู่ในเฟรมหลังปลดล็อกไปแล้ว
const unsigned long RESCAN_COOLDOWN_MS = 8000;

Servo doorServo;
httpd_handle_t webHttpd = NULL;
httpd_handle_t streamHttpd = NULL;
unsigned long lastUnlockAt = 0;

// กันการแย่งกล้องกันเองระหว่าง loop() (แสกนหน้า) กับ streamHandler() (ส่งภาพขึ้นเว็บ)
SemaphoreHandle_t cameraMutex;

// สถานะล่าสุดสำหรับแสดงบนเว็บ ผ่าน /status (อ่าน/เขียนต้องล็อก statusMutex เสมอ
// เพราะถูกแตะจากคนละ task กัน: loop() เขียน, /status handler อ่าน)
SemaphoreHandle_t statusMutex;
char lastStatusName[32] = "-";
float lastStatusSimilarity = 0;
unsigned long lastStatusAt = 0;
bool lastStatusUnlocked = false;

void unlockDoor(const char *reason);

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="th">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>กล้องห้องโถง - แสกนใบหน้าเปิดประตู</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; margin: 0; padding: 16px; }
    img { max-width: 100%; border-radius: 8px; margin-top: 12px; image-rendering: pixelated; }
    button {
      margin-top: 20px; padding: 16px 32px; font-size: 18px; border: none;
      border-radius: 8px; background: #2e7d32; color: white; cursor: pointer;
    }
    button:active { background: #1b5e20; }
    #status { margin-top: 12px; font-size: 14px; color: #9e9e9e; }
  </style>
</head>
<body>
  <h2>กล้องห้องโถง — แสกนใบหน้าอัตโนมัติ</h2>
  <img id="stream" alt="กล้องสด">
  <br>
  <div id="faceStatus" style="margin-top:12px; font-size:16px;">กำลังโหลดสถานะ...</div>
  <button onclick="openDoor()">เปิดประตู (สำรอง)</button>
  <div id="status"></div>
  <script>
    document.getElementById('stream').src =
      'http://' + window.location.hostname + ':81/stream';

    function openDoor() {
      document.getElementById('status').innerText = 'กำลังสั่งเปิดประตู...';
      fetch('/open-door')
        .then(r => r.text())
        .then(t => document.getElementById('status').innerText = t)
        .catch(e => document.getElementById('status').innerText = 'ผิดพลาด: ' + e);
    }

    function refreshFaceStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(s => {
          const el = document.getElementById('faceStatus');
          if (s.name === '-') {
            el.innerText = 'ยังไม่เจอใบหน้า';
          } else if (s.name === 'unknown') {
            el.innerText = 'เจอใบหน้าแต่จำไม่ได้ (ความมั่นใจ ' + s.similarity.toFixed(2) + ') — ' + s.secondsAgo + ' วิที่แล้ว';
          } else {
            el.innerText = (s.unlocked ? '✅ ปลดล็อกให้: ' : 'จำได้ว่าเป็น: ') + s.name +
              ' (ความมั่นใจ ' + s.similarity.toFixed(2) + ') — ' + s.secondsAgo + ' วิที่แล้ว';
          }
        })
        .catch(() => {});
    }
    setInterval(refreshFaceStatus, 1500);
    refreshFaceStatus();
  </script>
</body>
</html>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t streamHandler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  char part_buf[64];

  while (true) {
    // ไม่เรียก esp_camera_fb_get() เองที่นี่ — กล้องมี buffer เดียว (fb_count=1)
    // และ camera.capture() ในตัวไลบรารีจะยึด frame ค้างไว้จนกว่าจะ capture() รอบถัดไป
    // ถ้ามาขอเฟรมเองซ้อนอีกทาง จะรอกล้องว่างจนตาย (deadlock กับ loop() ที่ถือ mutex อยู่)
    // ทางที่ปลอดภัยคือ "แอบอ่าน" เฟรมล่าสุดที่ camera.frame ถืออยู่แล้วแทน
    uint8_t *jpegCopy = NULL;
    size_t jpegLen = 0;

    xSemaphoreTake(cameraMutex, portMAX_DELAY);
    if (camera.frame != NULL && camera.frame->len > 0) {
      jpegLen = camera.frame->len;
      jpegCopy = (uint8_t *)malloc(jpegLen);
      if (jpegCopy != NULL) {
        memcpy(jpegCopy, camera.frame->buf, jpegLen);
      }
    }
    xSemaphoreGive(cameraMutex);

    if (jpegCopy == NULL) {
      delay(50);  // ยังไม่มีเฟรมพร้อม (เช่นเพิ่งเปิดเครื่อง) รอสักครู่แล้วลองใหม่
      continue;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpegLen);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)jpegCopy, jpegLen);
    }

    free(jpegCopy);

    if (res != ESP_OK) break;

    delay(20);  // กันไม่ให้ขอเฟรมถี่จน loop() ไม่มีจังหวะแทรกเลย
  }
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  char json[160];

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  unsigned long secondsAgo = (millis() - lastStatusAt) / 1000;
  snprintf(json, sizeof(json),
           "{\"name\":\"%s\",\"similarity\":%.2f,\"unlocked\":%s,\"secondsAgo\":%lu}",
           lastStatusName, lastStatusSimilarity,
           lastStatusUnlocked ? "true" : "false", secondsAgo);
  xSemaphoreGive(statusMutex);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t openDoorHandler(httpd_req_t *req) {
  unlockDoor("ปุ่มเว็บ (สำรอง)");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(req, "เปิดประตูแล้ว! ล็อกกลับอัตโนมัติหลัง 4 วินาที");
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t openDoorUri = {"/open-door", HTTP_GET, openDoorHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &openDoorUri);
    httpd_register_uri_handler(webHttpd, &statusUri);
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, NULL};

  if (httpd_start(&streamHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ห้องโถง: แสกนใบหน้าอัตโนมัติ + เว็บ =====");

  cameraMutex = xSemaphoreCreateMutex();
  statusMutex = xSemaphoreCreateMutex();

  camera.pinout.wroom_s3();
  camera.brownout.disable();
  camera.resolution.face();
  camera.quality.high();

  detection.accurate();
  detection.confidence(0.7);
  // เพิ่มจาก 0.85 เป็น 0.93 หลังพบว่า 0.85 หลวมเกินไป (คนที่ไม่เคย enroll ถูกจำผิด)
  // ทดสอบเพิ่มเติม (2026-08-01) พบว่าแม้มุมกล้อง frontal ที่ดีที่สุดก็ได้แค่ ~0.90
  // ปรับลงเป็น 0.87 (ต่ำกว่าค่าดีที่สุดที่วัดได้จริงเล็กน้อย) — ต้องใช้งานด้วยมุมกล้อง
  // แบบ frontal เท่านั้นถึงจะแม่นยำดี
  recognition.confidence(0.87);

  while (!camera.begin().isOk())
    Serial.println(camera.exception.toString());

  while (!recognition.begin().isOk())
    Serial.println(recognition.exception.toString());

  Serial.println(">>> CAMERA + FACE RECOGNIZER OK <<<");
  Serial.println("(ใบหน้าที่ enroll ไว้ก่อนหน้ายังอยู่ครบใน NVS)");

  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_ANGLE);
  Serial.printf("Servo attach ที่ GPIO%d แล้ว (ล็อกอยู่)\n", SERVO_PIN);

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

  Serial.println("เริ่มแสกนใบหน้าอัตโนมัติ...");
}

void loop() {
  // ต้องมี delay ทุกรอบ ไม่งั้น loop() นี้จะวนขอ/ปล่อย cameraMutex ถี่จนแย่งคิวจาก
  // streamHandler() (คนละ task) ตลอด ทำให้ /stream ไม่ได้เฟรมไปส่งเลยสักครั้ง
  delay(15);

  // ล็อกกล้องตลอดช่วง capture+detect+recognize กันไม่ให้ streamHandler() (คนละ task)
  // มาแย่งเฟรมกลางคันจนแสกนหน้าเพี้ยนตอนมีคนเปิดเว็บดูภาพสดค้างไว้
  xSemaphoreTake(cameraMutex, portMAX_DELAY);

  bool faceFound = camera.capture().isOk() && recognition.detect().isOk();
  if (!faceFound) {
    xSemaphoreGive(cameraMutex);
    return;
  }

  if (millis() - lastUnlockAt < RESCAN_COOLDOWN_MS) {
    xSemaphoreGive(cameraMutex);
    return;
  }

  bool recognizeOk = recognition.recognize().isOk();
  xSemaphoreGive(cameraMutex);

  if (!recognizeOk)
    return;

  Serial.printf(
      "จำแนกได้ว่าเป็น: %s | ความมั่นใจ: %.2f\n",
      recognition.match.name.c_str(),
      recognition.match.similarity);

  // recognize().isOk() คืน true เสมอแม้จำไม่ได้ (name จะเป็น "unknown")
  // ต้องเช็คชื่อ + ค่าความมั่นใจเองว่าผ่านเกณฑ์จริงก่อนสั่งปลดล็อก
  bool isKnownPerson = recognition.match.name != "unknown" &&
                        recognition.match.similarity >= 0.87;

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  strncpy(lastStatusName, recognition.match.name.c_str(), sizeof(lastStatusName) - 1);
  lastStatusName[sizeof(lastStatusName) - 1] = '\0';
  lastStatusSimilarity = recognition.match.similarity;
  lastStatusAt = millis();
  lastStatusUnlocked = isKnownPerson;
  xSemaphoreGive(statusMutex);

  if (isKnownPerson) {
    unlockDoor(recognition.match.name.c_str());
  }
}

void unlockDoor(const char *reason) {
  Serial.printf(">>> ปลดล็อกประตู (%s) <<<\n", reason);
  doorServo.write(UNLOCK_ANGLE);
  delay(DOOR_OPEN_MS);
  doorServo.write(LOCK_ANGLE);
  Serial.println(">>> ล็อกประตูกลับแล้ว <<<");
  lastUnlockAt = millis();
}
