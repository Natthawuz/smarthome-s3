/*
  phase5d_fall_detection.cpp
  -----------------------------------------------------------
  ตรวจจับการล้มด้วยโมเดล CNN ที่เทรนเอง (training/train_model.py) รันบนบอร์ดเอง
  ผ่านไลบรารี ArduTFLite (TensorFlow Lite Micro แบบ Arduino-friendly)

  จำแนกภาพเป็น 3 หมวด: standing / sitting / fallen — ถ้าจำแนกว่า "fallen"
  ติดต่อกันหลายเฟรม (กันสัญญาณเท็จ) จะสั่ง Buzzer ดังทันที

  เพิ่มเว็บดูภาพสด (/stream) + สถานะการจำแนกล่าสุด (/status) เพื่อ debug ว่า
  กล้องเห็นภาพอะไรจริงๆ

  หมายเหตุสำคัญ: เดิมจับภาพแบบ RGB565 ดิบที่ 96x96 ตรงๆ แต่พบว่า OV2640 ร่วมกับ
  WiFi เปิดพร้อมกันไม่เสถียรกับโหมด raw (ภาพเพี้ยน/ค้างซ้ำ เป็นปัญหาที่มีรายงาน
  กันทั่วไป) จึงเปลี่ยนมาจับภาพเป็น JPEG แล้วถอดรหัส+ย่อขนาดเป็น 96x96 ด้วยซอฟต์แวร์
  ก่อนป้อนโมเดล

  หมายเหตุ (แก้ครั้งที่ 2 หลังพบว่าทำนายไม่แม่นทั้งที่ validation 97%):
  ตอนเก็บภาพเทรน (phase5b) ถ่ายที่ SVGA (800x600) แล้ว Python resize เป็น 96x96
  ด้วย bicubic (เนียน) — แต่โค้ดเดิมตรงนี้ถ่ายแค่ QVGA (320x240) แล้วย่อแบบ
  nearest-neighbor (หยาบ) ทำให้ภาพที่ป้อนโมเดลจริงต่างจากภาพตอนเทรนมาก
  (train/inference mismatch) จึงเปลี่ยนมาถ่าย SVGA ให้ตรงกับตอนเทรน และย่อขนาด
  ด้วย box-filter (เฉลี่ยพิกเซลในกรอบ) แทน nearest-neighbor ให้ใกล้เคียง bicubic
  มากขึ้น

  พิน Buzzer: IO2 (พินว่างที่เหลือ ไม่ชนกล้อง/servo/MPU-6050/SD)
  โมเดล: 96x96 RGB, float32 (ArduTFLite รองรับแค่ float32 ไม่ใช่ INT8 quantized)
  ต้องมีไฟล์ include/wifi_credentials.h ก่อน (เหมือน phase3/phase4/phase5b)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <ArduTFLite.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "camera_pins.h"
#include "fall_model_data.h"
#include "wifi_credentials.h"

#define BUZZER_PIN 2

const char *LABELS[] = {"standing", "sitting", "fallen"};
const int NUM_LABELS = 3;
const int FALLEN_INDEX = 2;
const float CONFIDENCE_THRESHOLD = 0.7;
const int CONSECUTIVE_NEEDED = 3;  // กันสัญญาณเท็จจากเฟรมเดียว
const unsigned long BUZZER_DURATION_MS = 5000;

constexpr int TENSOR_ARENA_SIZE = 500 * 1024;  // โมเดลต้องการจริง ~384KB จากที่เคยทดสอบ
byte *tensorArena = nullptr;

int consecutiveFallenCount = 0;

// ----- แชร์ภาพ/สถานะล่าสุดระหว่าง loop() กับ web handler -----
SemaphoreHandle_t dataMutex;
uint8_t *latestJpegBuf = nullptr;
size_t latestJpegLen = 0;
char latestLabel[16] = "-";
float latestScores[NUM_LABELS] = {0, 0, 0};

httpd_handle_t webHttpd = NULL;
httpd_handle_t streamHttpd = NULL;

void initCamera() {
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
  config.pixel_format = PIXFORMAT_JPEG;   // ใช้ JPEG แบบเดียวกับเฟสอื่นๆ ที่เสถียร
  config.frame_size   = FRAMESIZE_SVGA;   // 800x600 ตรงกับความละเอียดตอนเก็บภาพเทรน (phase5b)
  config.jpeg_quality  = 10;              // ตรงกับตอนเก็บภาพเทรน (phase5b)
  config.fb_count      = 2;
  config.fb_location   = CAMERA_FB_IN_PSRAM;
  config.grab_mode     = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("!!! CAMERA INIT FAILED (error 0x%x) !!!\n", err);
    while (true) delay(1000);
  }
  Serial.println(">>> CAMERA OK <<<");
}

const int MODEL_SIZE = 96;
uint8_t *rgb888Buf = nullptr;  // buffer ถอดรหัส JPEG -> RGB888 ที่ความละเอียดกล้องจริง (จอง PSRAM ครั้งเดียวใน setup)

// ถอดรหัสเฟรม JPEG เป็น RGB888 แล้วย่อขนาดเป็น 96x96 ด้วย box-filter (เฉลี่ยพิกเซล
// ในกรอบที่แมปมาจากภาพต้นฉบับ) ใกล้เคียงกับ bicubic resize ที่ใช้ตอนเทรนมากกว่า
// nearest-neighbor แบบเดิม — ป้อนเข้าโมเดลเป็น float normalize 0-1 ทีละพิกเซล
bool feedFrameToModel(camera_fb_t *fb) {
  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb888Buf)) {
    Serial.println("ถอดรหัส JPEG ไม่สำเร็จ");
    return false;
  }

  int srcW = fb->width;
  int srcH = fb->height;

  for (int oy = 0; oy < MODEL_SIZE; oy++) {
    int sy0 = oy * srcH / MODEL_SIZE;
    int sy1 = (oy + 1) * srcH / MODEL_SIZE;
    if (sy1 <= sy0) sy1 = sy0 + 1;

    for (int ox = 0; ox < MODEL_SIZE; ox++) {
      int sx0 = ox * srcW / MODEL_SIZE;
      int sx1 = (ox + 1) * srcW / MODEL_SIZE;
      if (sx1 <= sx0) sx1 = sx0 + 1;

      uint32_t sumR = 0, sumG = 0, sumB = 0;
      int count = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          int srcIdx = (sy * srcW + sx) * 3;
          sumR += rgb888Buf[srcIdx + 0];
          sumG += rgb888Buf[srcIdx + 1];
          sumB += rgb888Buf[srcIdx + 2];
          count++;
        }
      }

      uint8_t avgR = sumR / count, avgG = sumG / count, avgB = sumB / count;

      // ห้ามหาร 255 ตรงนี้! โมเดล (train_model.py) มี tf.keras.layers.Rescaling(1/255)
      // เป็นเลเยอร์แรกอยู่ในตัวโมเดลเองแล้ว ถ้าหารซ้ำที่นี่อีกรอบ ค่าที่ป้อนเข้าจริง
      // จะเหลือแค่ ~0-0.004 (แทบเป็น 0 หมด) ทำให้โมเดลแยกแยะภาพไม่ได้เลย (ค่า
      // output ค้างเกือบคงที่ไม่ว่าจะป้อนภาพอะไร) — ต้องป้อนค่าพิกเซลดิบ 0-255 ตรงๆ
      int dstIdx = (oy * MODEL_SIZE + ox) * 3;
      modelSetInput((float)avgR, dstIdx + 0);
      modelSetInput((float)avgG, dstIdx + 1);
      modelSetInput((float)avgB, dstIdx + 2);
    }
  }
  return true;
}

void triggerBuzzer(const char *reason) {
  Serial.printf(">>> ตรวจพบการล้ม! (%s) — Buzzer ดัง <<<\n", reason);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(BUZZER_DURATION_MS);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println(">>> Buzzer หยุดแล้ว <<<");
  consecutiveFallenCount = 0;
}

// ===================== เว็บดูภาพสด + สถานะ =====================

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
  <title>ตรวจจับการล้ม - ดูภาพสด</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; margin: 0; padding: 16px; }
    img { width: 320px; height: 320px; image-rendering: pixelated; border-radius: 8px; margin-top: 12px; background: #222; }
    #status { margin-top: 16px; font-size: 18px; }
    .bar { display: inline-block; width: 140px; text-align: left; }
  </style>
</head>
<body>
  <h2>กล้องห้องโถง — ตรวจจับการล้ม (ภาพ 96x96 ที่ป้อนโมเดลจริง)</h2>
  <img id="stream" alt="กล้องสด">
  <div id="status">กำลังโหลด...</div>
  <script>
    document.getElementById('stream').src =
      'http://' + window.location.hostname + ':81/stream';

    function refresh() {
      fetch('/status').then(r => r.json()).then(s => {
        document.getElementById('status').innerHTML =
          '<div class="bar">ยืน: ' + (s.standing * 100).toFixed(0) + '%</div><br>' +
          '<div class="bar">นั่ง: ' + (s.sitting * 100).toFixed(0) + '%</div><br>' +
          '<div class="bar">ล้ม: ' + (s.fallen * 100).toFixed(0) + '%</div><br>' +
          '<b>ทำนาย: ' + s.label + '</b>';
      }).catch(() => {});
    }
    setInterval(refresh, 500);
    refresh();
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
    uint8_t *jpegCopy = NULL;
    size_t jpegLen = 0;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (latestJpegBuf != nullptr && latestJpegLen > 0) {
      jpegLen = latestJpegLen;
      jpegCopy = (uint8_t *)malloc(jpegLen);
      if (jpegCopy != NULL) {
        memcpy(jpegCopy, latestJpegBuf, jpegLen);
      }
    }
    xSemaphoreGive(dataMutex);

    if (jpegCopy == NULL) {
      delay(50);
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
    delay(80);
  }
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  char json[160];
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  snprintf(json, sizeof(json),
           "{\"standing\":%.3f,\"sitting\":%.3f,\"fallen\":%.3f,\"label\":\"%s\"}",
           latestScores[0], latestScores[1], latestScores[2], latestLabel);
  xSemaphoreGive(dataMutex);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
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
  Serial.println("===== ตรวจจับการล้มด้วยโมเดลที่เทรนเอง =====");

  dataMutex = xSemaphoreCreateMutex();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  initCamera();

  // จองพื้นที่ถอดรหัส JPEG -> RGB888 ที่ความละเอียด SVGA (800x600x3 bytes)
  rgb888Buf = (uint8_t *)heap_caps_malloc(800 * 600 * 3, MALLOC_CAP_SPIRAM);
  if (rgb888Buf == nullptr) {
    Serial.println("!!! จอง rgb888Buf ไม่สำเร็จ (PSRAM ไม่พอ) !!!");
    while (true) delay(1000);
  }

  // จองพื้นที่ tensor arena จาก PSRAM (โมเดลใหญ่กว่า RAM ภายในจะรองรับไหว)
  tensorArena = (byte *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM);
  if (tensorArena == nullptr) {
    Serial.println("!!! จอง tensor arena ไม่สำเร็จ (PSRAM ไม่พอ) !!!");
    while (true) delay(1000);
  }

  Serial.println("กำลังโหลดโมเดล...");
  if (!modelInit(fall_model_data, tensorArena, TENSOR_ARENA_SIZE)) {
    Serial.println("!!! โหลดโมเดลไม่สำเร็จ !!!");
    while (true) delay(1000);
  }
  Serial.println(">>> โหลดโมเดลสำเร็จ <<<");

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
  Serial.println("เริ่มตรวจจับ...");
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("ถ่ายภาพไม่สำเร็จ");
    delay(200);
    return;
  }

  bool ok = feedFrameToModel(fb);

  // fb เป็น JPEG จากกล้องอยู่แล้ว (ไม่ต้องแปลง) ก็อปไว้ส่งขึ้นเว็บ /stream
  uint8_t *jpegCopy = (uint8_t *)malloc(fb->len);
  size_t jpegLen = fb->len;
  if (jpegCopy != nullptr) {
    memcpy(jpegCopy, fb->buf, fb->len);
  }

  esp_camera_fb_return(fb);

  if (jpegCopy != nullptr) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (latestJpegBuf != nullptr) free(latestJpegBuf);
    latestJpegBuf = jpegCopy;
    latestJpegLen = jpegLen;
    xSemaphoreGive(dataMutex);
  }

  if (!ok) {
    delay(50);
    return;
  }

  if (!modelRunInference()) {
    Serial.println("รัน inference ไม่สำเร็จ");
    return;
  }

  float scores[NUM_LABELS];
  int bestIndex = 0;
  for (int i = 0; i < NUM_LABELS; i++) {
    scores[i] = modelGetOutput(i);
    if (scores[i] > scores[bestIndex]) bestIndex = i;
  }

  Serial.printf("ยืน=%.2f นั่ง=%.2f ล้ม=%.2f -> %s\n",
                scores[0], scores[1], scores[2], LABELS[bestIndex]);

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < NUM_LABELS; i++) latestScores[i] = scores[i];
  strncpy(latestLabel, LABELS[bestIndex], sizeof(latestLabel) - 1);
  latestLabel[sizeof(latestLabel) - 1] = '\0';
  xSemaphoreGive(dataMutex);

  if (bestIndex == FALLEN_INDEX && scores[FALLEN_INDEX] >= CONFIDENCE_THRESHOLD) {
    consecutiveFallenCount++;
    if (consecutiveFallenCount >= CONSECUTIVE_NEEDED) {
      char reason[32];
      snprintf(reason, sizeof(reason), "confidence %.2f", scores[FALLEN_INDEX]);
      triggerBuzzer(reason);
    }
  } else {
    consecutiveFallenCount = 0;
  }

  delay(50);
}
