/*
  phase3_web_camera_door.cpp
  -----------------------------------------------------------
  เว็บเซิร์ฟเวอร์บน ESP32-S3 เอง สำหรับ:
  1. ดูภาพสดจากกล้อง (MJPEG stream) ผ่านเบราว์เซอร์บนมือถือ/คอม
  2. ปุ่ม "เปิดประตู" บนหน้าเว็บ สั่ง servo ปลดล็อกจากระยะไกลผ่าน WiFi

  ต้องมีไฟล์ include/wifi_credentials.h ก่อน (คัดลอกจาก
  wifi_credentials.h.example แล้วใส่ SSID/PASSWORD จริง)

  วิธีใช้:
  1. อัปโหลดแล้วเปิด Serial Monitor 115200 — รอจนขึ้น IP address
  2. เอามือถือ/คอมที่ต่อ WiFi วงเดียวกัน เปิดเบราว์เซอร์ไปที่ IP นั้น (เช่น http://192.168.1.50)
  3. จะเห็นภาพสดจากกล้อง + ปุ่ม "เปิดประตู" กดแล้ว servo จะปลดล็อกค้าง 4 วิ แล้วล็อกกลับ

  หมายเหตุ: ความละเอียด VGA (640x480) ไม่ใช่ 240x240 แบบไฟล์ face recognition
  เพราะไฟล์นี้เน้นดูภาพสด ไม่ได้ทำการจดจำใบหน้า (จะรวมสองอย่างเข้าด้วยกันตอนเฟส
  "รวมเป็นระบบเดียว" ทีหลัง)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_camera.h>
#include <ESP32Servo.h>
#include "camera_pins.h"
#include "hall_pins.h"
#include "wifi_credentials.h"

const int LOCK_ANGLE = 0;
const int UNLOCK_ANGLE = 90;
const unsigned long DOOR_OPEN_MS = 4000;

Servo doorServo;
httpd_handle_t cameraHttpd = NULL;
httpd_handle_t streamHttpd = NULL;

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
  <title>กล้องห้องโถง - บ้านจำลองผู้สูงอายุ</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; margin: 0; padding: 16px; }
    img { max-width: 100%; border-radius: 8px; margin-top: 12px; }
    button {
      margin-top: 20px; padding: 16px 32px; font-size: 18px; border: none;
      border-radius: 8px; background: #2e7d32; color: white; cursor: pointer;
    }
    button:active { background: #1b5e20; }
    #status { margin-top: 12px; font-size: 14px; color: #9e9e9e; }
  </style>
</head>
<body>
  <h2>กล้องห้องโถง</h2>
  <img id="stream" alt="กล้องสด">
  <br>
  <button onclick="openDoor()">เปิดประตู</button>
  <div id="status"></div>
  <script>
    // /stream อยู่บนเซิร์ฟเวอร์คนละ port (81) จากหน้าเว็บหลัก (80)
    // ต้องต่อ URL แบบเต็มด้วย hostname ปัจจุบัน ใช้ src="/stream" เฉยๆ ไม่ได้
    document.getElementById('stream').src =
      'http://' + window.location.hostname + ':81/stream';

    function openDoor() {
      document.getElementById('status').innerText = 'กำลังสั่งเปิดประตู...';
      fetch('/open-door')
        .then(r => r.text())
        .then(t => document.getElementById('status').innerText = t)
        .catch(e => document.getElementById('status').innerText = 'ผิดพลาด: ' + e);
    }
  </script>
</body>
</html>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t streamHandler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  char part_buf[64];

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("ถ่ายภาพไม่สำเร็จ (stream)");
      res = ESP_FAIL;
      break;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t openDoorHandler(httpd_req_t *req) {
  Serial.println(">>> สั่งเปิดประตูจากเว็บ <<<");
  doorServo.write(UNLOCK_ANGLE);
  delay(DOOR_OPEN_MS);
  doorServo.write(LOCK_ANGLE);
  Serial.println(">>> ล็อกประตูกลับแล้ว <<<");

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(req, "เปิดประตูแล้ว! ล็อกกลับอัตโนมัติหลัง 4 วินาที");
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t openDoorUri = {"/open-door", HTTP_GET, openDoorHandler, NULL};

  if (httpd_start(&cameraHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(cameraHttpd, &indexUri);
    httpd_register_uri_handler(cameraHttpd, &openDoorUri);
  }

  // เปิดพอร์ตแยกสำหรับ /stream (ตามแบบฉบับของ Espressif CameraWebServer)
  // กันไม่ให้การรอ stream ค้าง ไปบล็อกการเรียก /open-door
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
  Serial.println("===== เว็บเซิร์ฟเวอร์กล้อง + ปุ่มเปิดประตู =====");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;   // 640x480 พอดูสดผ่านเว็บ
  config.jpeg_quality  = 12;
  config.fb_count      = psramFound() ? 2 : 1;
  config.fb_location   = CAMERA_FB_IN_PSRAM;
  config.grab_mode     = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("!!! CAMERA INIT FAILED (error 0x%x) !!!\n", err);
    return;
  }
  Serial.println(">>> CAMERA OK <<<");

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
  Serial.println(">>> WiFi เชื่อมต่อสำเร็จ <<<");
  Serial.print("เปิดเบราว์เซอร์ไปที่: http://");
  Serial.println(WiFi.localIP());

  startCameraServer();
}

void loop() {
  delay(10000);
}
