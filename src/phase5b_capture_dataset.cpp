/*
  phase5b_capture_dataset.cpp
  -----------------------------------------------------------
  เว็บเก็บภาพสอนโมเดลตรวจจับการล้ม บันทึกลง SD card (พินยืนยันแล้วใน Phase 5A)
  แบ่งเป็น 3 หมวด: ยืน (standing) / นั่ง (sitting) / ล้ม (fallen)

  วิธีใช้:
  1. เอาบอร์ดไปติดตั้งที่ตำแหน่ง/มุมกล้องจริงที่จะใช้งานจริงในห้องโถง (สำคัญมาก —
     โมเดลต้องเทรนจากมุมกล้องจริง ไม่ใช่ถ่ายจากมุมอื่นแล้วเอามาสอน)
  2. เปิด Serial Monitor ดู IP แล้วเปิดเบราว์เซอร์ไปที่ IP นั้น
  3. จะเห็นภาพสดจากกล้อง + ปุ่ม 3 ปุ่ม (ยืน/นั่ง/ล้ม) พร้อมตัวนับจำนวนภาพที่เก็บแล้ว
  4. โพสท่าตามปุ่มที่จะกด แล้วกดปุ่มนั้น ระบบจะถ่ายภาพแล้วเซฟลง SD ทันที
     ทำซ้ำหลายสิบครั้งต่อท่า สลับมุม/ระยะ/แสงบ้างเพื่อให้โมเดลเรียนรู้ได้กว้างขึ้น
  5. เสร็จแล้วถอด SD การ์ดไปอ่านที่คอม จะเจอโฟลเดอร์ /dataset/standing,
     /dataset/sitting, /dataset/fallen พร้อมภาพ JPEG ให้เอาไปอัปโหลดเทรนที่
     Edge Impulse Studio ต่อ (เฟส C)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "FS.h"
#include "SD_MMC.h"
#include "camera_pins.h"
#include "sd_pins.h"
#include "wifi_credentials.h"

httpd_handle_t webHttpd = NULL;
httpd_handle_t streamHttpd = NULL;
SemaphoreHandle_t cameraMutex;

int countStanding = 0;
int countSitting = 0;
int countFallen = 0;

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
  <title>เก็บภาพสอนโมเดล - ตรวจจับการล้ม</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; margin: 0; padding: 16px; }
    img { max-width: 100%; border-radius: 8px; margin-top: 12px; }
    .buttons { display: flex; gap: 10px; justify-content: center; margin-top: 20px; flex-wrap: wrap; }
    button {
      padding: 18px 24px; font-size: 18px; border: none; border-radius: 8px;
      color: white; cursor: pointer; min-width: 100px;
    }
    #btnStanding { background: #1565c0; }
    #btnSitting  { background: #f9a825; }
    #btnFallen   { background: #c62828; }
    button:active { filter: brightness(0.8); }
    #status { margin-top: 14px; font-size: 15px; color: #9e9e9e; }
    .counts { margin-top: 10px; font-size: 14px; }
  </style>
</head>
<body>
  <h2>เก็บภาพสอนโมเดลตรวจจับการล้ม</h2>
  <img id="stream" alt="กล้องสด">
  <div class="counts">
    ยืน: <span id="cStanding">-</span> |
    นั่ง: <span id="cSitting">-</span> |
    ล้ม: <span id="cFallen">-</span>
  </div>
  <div class="buttons">
    <button id="btnStanding" onclick="capture('standing')">ยืน</button>
    <button id="btnSitting" onclick="capture('sitting')">นั่ง</button>
    <button id="btnFallen" onclick="capture('fallen')">ล้ม</button>
  </div>
  <div id="status"></div>
  <script>
    document.getElementById('stream').src =
      'http://' + window.location.hostname + ':81/stream';

    function updateCounts(c) {
      document.getElementById('cStanding').innerText = c.standing;
      document.getElementById('cSitting').innerText = c.sitting;
      document.getElementById('cFallen').innerText = c.fallen;
    }

    function refreshCounts() {
      fetch('/counts').then(r => r.json()).then(updateCounts).catch(() => {});
    }

    function capture(label) {
      document.getElementById('status').innerText = 'กำลังถ่ายภาพ...';
      fetch('/capture?label=' + label)
        .then(r => r.json())
        .then(j => {
          if (j.ok) {
            document.getElementById('status').innerText = 'บันทึกแล้ว: ' + label;
            updateCounts(j.counts);
          } else {
            document.getElementById('status').innerText = 'ผิดพลาด: ' + j.error;
          }
        })
        .catch(e => document.getElementById('status').innerText = 'ผิดพลาด: ' + e);
    }

    refreshCounts();
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
    camera_fb_t *fb = NULL;

    xSemaphoreTake(cameraMutex, portMAX_DELAY);
    fb = esp_camera_fb_get();
    xSemaphoreGive(cameraMutex);

    if (!fb) {
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

    xSemaphoreTake(cameraMutex, portMAX_DELAY);
    esp_camera_fb_return(fb);
    xSemaphoreGive(cameraMutex);

    if (res != ESP_OK) break;
    delay(20);
  }
  return res;
}

const char *labelToDir(const String &label) {
  if (label == "standing") return "/dataset/standing";
  if (label == "sitting") return "/dataset/sitting";
  if (label == "fallen") return "/dataset/fallen";
  return NULL;
}

int *labelToCounter(const String &label) {
  if (label == "standing") return &countStanding;
  if (label == "sitting") return &countSitting;
  if (label == "fallen") return &countFallen;
  return NULL;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  char query[64];
  char labelBuf[16] = "";
  String label;

  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "label", labelBuf, sizeof(labelBuf));
  }
  label = String(labelBuf);

  const char *dir = labelToDir(label);
  int *counter = labelToCounter(label);

  httpd_resp_set_type(req, "application/json; charset=utf-8");

  if (dir == NULL || counter == NULL) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid label\"}");
  }

  xSemaphoreTake(cameraMutex, portMAX_DELAY);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(cameraMutex);
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"capture failed\"}");
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/img_%lu.jpg", dir, millis());

  File f = SD_MMC.open(path, FILE_WRITE);
  bool writeOk = false;
  if (f) {
    writeOk = f.write(fb->buf, fb->len) == fb->len;
    f.close();
  }

  esp_camera_fb_return(fb);
  xSemaphoreGive(cameraMutex);

  if (!writeOk) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd write failed\"}");
  }

  (*counter)++;
  Serial.printf("บันทึกภาพ: %s (รวม %d ภาพ)\n", path, *counter);

  char json[160];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"counts\":{\"standing\":%d,\"sitting\":%d,\"fallen\":%d}}",
           countStanding, countSitting, countFallen);
  return httpd_resp_sendstr(req, json);
}

static esp_err_t countsHandler(httpd_req_t *req) {
  char json[128];
  snprintf(json, sizeof(json),
           "{\"standing\":%d,\"sitting\":%d,\"fallen\":%d}",
           countStanding, countSitting, countFallen);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t captureUri = {"/capture", HTTP_GET, captureHandler, NULL};
  httpd_uri_t countsUri = {"/counts", HTTP_GET, countsHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &captureUri);
    httpd_register_uri_handler(webHttpd, &countsUri);
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, NULL};

  if (httpd_start(&streamHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
  }
}

int countFilesInDir(const char *dir) {
  int count = 0;
  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) return 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) count++;
    f = root.openNextFile();
  }
  return count;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== เก็บภาพสอนโมเดลตรวจจับการล้ม =====");

  cameraMutex = xSemaphoreCreateMutex();

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
  config.frame_size   = FRAMESIZE_SVGA;  // 800x600 คมพอสำหรับเก็บ dataset
  config.jpeg_quality  = 10;
  config.fb_count      = psramFound() ? 2 : 1;
  config.fb_location   = CAMERA_FB_IN_PSRAM;
  config.grab_mode     = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("!!! CAMERA INIT FAILED (error 0x%x) !!!\n", err);
    return;
  }
  Serial.println(">>> CAMERA OK <<<");

  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("!!! SD MOUNT FAILED !!!");
    return;
  }
  Serial.println(">>> SD OK <<<");

  SD_MMC.mkdir("/dataset");
  SD_MMC.mkdir("/dataset/standing");
  SD_MMC.mkdir("/dataset/sitting");
  SD_MMC.mkdir("/dataset/fallen");

  countStanding = countFilesInDir("/dataset/standing");
  countSitting = countFilesInDir("/dataset/sitting");
  countFallen = countFilesInDir("/dataset/fallen");
  Serial.printf("จำนวนภาพเดิมใน SD: ยืน=%d นั่ง=%d ล้ม=%d\n",
                countStanding, countSitting, countFallen);

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
}

void loop() {
  delay(1000);
}
