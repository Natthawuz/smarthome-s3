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

  ทางเลือก: ถ้าคอมไม่มีช่องอ่าน SD (เช่น MacBook รุ่นใหม่) ใช้ส่วน "อัปโหลดไฟล์ภาพ"
  ท้ายหน้าเว็บแทนได้ — เลือกไฟล์ภาพจากคอม/มือถือ (เลือกได้หลายไฟล์พร้อมกัน) เลือกหมวด
  แล้วกดอัปโหลด ระบบจะส่งไฟล์ผ่าน WiFi ไปเซฟลง SD ในบอร์ดให้ตรงโฟลเดอร์ทันที
  ไม่ต้องถอดการ์ดเลย
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

  <hr style="margin-top:24px; border-color:#333;">
  <h3>หรืออัปโหลดไฟล์ภาพจากคอม/มือถือ</h3>
  <div>
    <select id="uploadLabel">
      <option value="standing">ยืน</option>
      <option value="sitting">นั่ง</option>
      <option value="fallen">ล้ม</option>
    </select>
    <input type="file" id="uploadFiles" accept="image/*" multiple>
  </div>
  <button style="margin-top:12px; background:#555;" onclick="uploadFiles()">อัปโหลด</button>
  <div id="uploadStatus" style="margin-top:10px; font-size:14px; color:#9e9e9e;"></div>

  <hr style="margin-top:24px; border-color:#333;">
  <button style="background:#8b0000;" onclick="resetAll()">ลบภาพทั้งหมด (เริ่มใหม่)</button>

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

    async function uploadFiles() {
      const label = document.getElementById('uploadLabel').value;
      const files = document.getElementById('uploadFiles').files;
      const statusEl = document.getElementById('uploadStatus');

      if (files.length === 0) {
        statusEl.innerText = 'ยังไม่ได้เลือกไฟล์';
        return;
      }

      for (let i = 0; i < files.length; i++) {
        statusEl.innerText = `กำลังอัปโหลด ${i + 1}/${files.length}: ${files[i].name}`;
        try {
          const r = await fetch('/upload?label=' + label, {
            method: 'POST',
            body: files[i]
          });
          const j = await r.json();
          if (j.ok) {
            updateCounts(j.counts);
          } else {
            statusEl.innerText = 'ผิดพลาดที่ไฟล์ ' + files[i].name + ': ' + j.error;
            return;
          }
        } catch (e) {
          statusEl.innerText = 'ผิดพลาดที่ไฟล์ ' + files[i].name + ': ' + e;
          return;
        }
      }

      statusEl.innerText = `อัปโหลดสำเร็จ ${files.length} ไฟล์!`;
    }

    function resetAll() {
      if (!confirm('ลบภาพทั้งหมดในทุกหมวดจริงหรือไม่? กู้คืนไม่ได้')) return;
      document.getElementById('status').innerText = 'กำลังลบ...';
      fetch('/reset-all')
        .then(r => r.json())
        .then(j => {
          if (j.ok) {
            document.getElementById('status').innerText = 'ลบทั้งหมดแล้ว';
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
    Serial.printf("!!! capture failed — heap ว่าง=%u, PSRAM ว่าง=%u !!!\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
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

static esp_err_t uploadHandler(httpd_req_t *req) {
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

  if (req->content_len <= 0 || req->content_len > 15 * 1024 * 1024) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad content length\"}");
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/upload_%lu.jpg", dir, millis());

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd open failed\"}");
  }

  // อัปโหลดจาก fetch(body: file) เป็น raw binary ตรงๆ ไม่ใช่ multipart/form-data
  // จึงแค่อ่านทีละ chunk แล้วเขียนลงไฟล์ต่อกันไปจนครบ content_len
  // ใช้ chunk ใหญ่ขึ้น (2KB) ลดจำนวนรอบเขียน SD ให้เร็วขึ้น กัน timeout ตอนไฟล์ใหญ่
  // (ต้องเพิ่ม stack_size ของ httpd ด้วย ไม่งั้น buffer นี้ทำให้ stack overflow)
  char buf[2048];
  int remaining = req->content_len;
  bool ok = true;

  while (remaining > 0) {
    int toRead = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int received = httpd_req_recv(req, buf, toRead);
    if (received <= 0) {
      ok = false;
      break;
    }
    if (f.write((const uint8_t *)buf, received) != (size_t)received) {
      ok = false;
      break;
    }
    remaining -= received;
  }
  f.close();

  if (!ok) {
    SD_MMC.remove(path);
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"upload write failed\"}");
  }

  (*counter)++;
  Serial.printf("อัปโหลดภาพ: %s (รวม %d ภาพ)\n", path, *counter);

  char json[160];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"counts\":{\"standing\":%d,\"sitting\":%d,\"fallen\":%d}}",
           countStanding, countSitting, countFallen);
  return httpd_resp_sendstr(req, json);
}

void deleteAllInDir(const char *dir) {
  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      char path[80];
      snprintf(path, sizeof(path), "%s/%s", dir, f.name());
      f.close();
      SD_MMC.remove(path);
    } else {
      f.close();
    }
    f = root.openNextFile();
  }
  root.close();
}

static esp_err_t resetAllHandler(httpd_req_t *req) {
  xSemaphoreTake(cameraMutex, portMAX_DELAY);
  deleteAllInDir("/dataset/standing");
  deleteAllInDir("/dataset/sitting");
  deleteAllInDir("/dataset/fallen");
  countStanding = 0;
  countSitting = 0;
  countFallen = 0;
  xSemaphoreGive(cameraMutex);

  Serial.println("ลบภาพทั้งหมดในทุกหมวดแล้ว");

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true,\"counts\":{\"standing\":0,\"sitting\":0,\"fallen\":0}}");
}

static esp_err_t countsHandler(httpd_req_t *req) {
  char json[128];
  snprintf(json, sizeof(json),
           "{\"standing\":%d,\"sitting\":%d,\"fallen\":%d}",
           countStanding, countSitting, countFallen);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

// คืนรายชื่อไฟล์ในหมวดที่ระบุ เป็น JSON array (ใช้คู่กับ /download เพื่อดึงภาพออกผ่าน WiFi)
static esp_err_t listHandler(httpd_req_t *req) {
  char query[64];
  char labelBuf[16] = "";
  String label;

  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "label", labelBuf, sizeof(labelBuf));
  }
  label = String(labelBuf);

  const char *dir = labelToDir(label);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (dir == NULL) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid label\"}");
  }

  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"open dir failed\"}");
  }

  httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"files\":[");
  File f = root.openNextFile();
  bool first = true;
  while (f) {
    if (!f.isDirectory()) {
      if (!first) httpd_resp_sendstr_chunk(req, ",");
      first = false;
      char item[96];
      snprintf(item, sizeof(item), "\"%s\"", f.name());
      httpd_resp_sendstr_chunk(req, item);
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  httpd_resp_sendstr_chunk(req, "]}");
  return httpd_resp_sendstr_chunk(req, NULL);
}

// ส่งไฟล์ภาพเดี่ยวๆ กลับเป็น image/jpeg ดิบ (ใช้ดึงภาพออกจาก SD ผ่าน WiFi ทีละไฟล์)
static esp_err_t downloadHandler(httpd_req_t *req) {
  char query[128];
  char labelBuf[16] = "";
  char fileBuf[64] = "";

  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "label", labelBuf, sizeof(labelBuf));
    httpd_query_key_value(query, "file", fileBuf, sizeof(fileBuf));
  }
  String label = String(labelBuf);
  const char *dir = labelToDir(label);

  if (dir == NULL || strlen(fileBuf) == 0 || strchr(fileBuf, '/') != NULL) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "bad request");
  }

  char path[96];
  snprintf(path, sizeof(path), "%s/%s", dir, fileBuf);

  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "not found");
  }

  httpd_resp_set_type(req, "image/jpeg");
  uint8_t buf[2048];
  int len;
  while ((len = f.read(buf, sizeof(buf))) > 0) {
    if (httpd_resp_send_chunk(req, (const char *)buf, len) != ESP_OK) {
      f.close();
      return ESP_FAIL;
    }
  }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.uri_match_fn = httpd_uri_match_wildcard;
  // อัปโหลดไฟล์ใหญ่ผ่าน WiFi ใช้เวลานานกว่า timeout ปกติ (ค่า default 5 วิ)
  // และ buffer 2KB ใน uploadHandler ต้องการ stack มากกว่า default (4KB)
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;
  config.stack_size = 8192;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t captureUri = {"/capture", HTTP_GET, captureHandler, NULL};
  httpd_uri_t countsUri = {"/counts", HTTP_GET, countsHandler, NULL};
  httpd_uri_t uploadUri = {"/upload", HTTP_POST, uploadHandler, NULL};
  httpd_uri_t resetAllUri = {"/reset-all", HTTP_GET, resetAllHandler, NULL};
  httpd_uri_t listUri = {"/list", HTTP_GET, listHandler, NULL};
  httpd_uri_t downloadUri = {"/download", HTTP_GET, downloadHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &captureUri);
    httpd_register_uri_handler(webHttpd, &countsUri);
    httpd_register_uri_handler(webHttpd, &uploadUri);
    httpd_register_uri_handler(webHttpd, &resetAllUri);
    httpd_register_uri_handler(webHttpd, &listUri);
    httpd_register_uri_handler(webHttpd, &downloadUri);
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
  // ไม่ใช้ psramFound() เช็ค เพราะพบว่าฟังก์ชันนี้รายงานผิด (false negative) บนบอร์ดนี้
  // ทั้งที่ PSRAM ยังใช้งานได้จริงผ่าน heap_caps_malloc(MALLOC_CAP_SPIRAM) ตรงๆ
  config.fb_count      = 2;
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
  Serial.printf(">>> PSRAM ว่างตอน boot เสร็จ: %u bytes (รวม %u bytes) <<<\n",
                ESP.getFreePsram(), ESP.getPsramSize());
}

void loop() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 3000) {
    Serial.printf("PSRAM ว่าง: %u bytes\n", ESP.getFreePsram());
    lastPrint = millis();
  }
  delay(1000);
}
