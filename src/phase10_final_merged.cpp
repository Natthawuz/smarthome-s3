/*
  phase10_final_merged.cpp
  -----------------------------------------------------------
  เฟิร์มแวร์สุดท้าย: รวมทุกห้อง/ทุกฟีเจอร์เข้าไฟล์เดียว รันพร้อมกันจริงบนบอร์ดเดียว
  (ก่อนหน้านี้แต่ละห้องเป็นไฟล์แยก สลับ flash ทีละไฟล์ — ตอนนี้รวมเป็นระบบเดียว)

  รวมจาก: phase4 (กล้อง+จดจำใบหน้า+ปลดล็อกประตู), phase5d (ตรวจจับการล้ม),
  phase6 (ห้องนอน DHT22+OLED), phase7 (ห้องน้ำ SOS), phase8 (แผ่นดินไหว MPU-6050),
  phase9 (ห้องครัว MQ-7+ปั๊มน้ำ)

  ===== การเปลี่ยนแปลงสำคัญตอนรวม (เทียบกับตอนแยกไฟล์) =====

  1) **GPIO14 ชนกัน**: SERVO_PIN (ห้องโถง) เดิมใช้ร่วมกับ SOS_BUTTON_PIN (ห้องน้ำ)
     ได้ตอนแยกไฟล์ เพราะรันคนละเฟส แต่รวมไฟล์แล้วรันพร้อมกัน ขาเดียวเป็นทั้ง output
     (servo) และ input (ปุ่ม) พร้อมกันไม่ได้ — ย้าย SOS_BUTTON_PIN ไป **GPIO40** แทน
     (ว่างจริงหลังถอด SD card) ดู include/unified_pins.h — ต้องย้ายสายจริงบนบอร์ด

  2) **I2C บัสชนกัน**: เดิม OLED (ห้องนอน) กับ MPU-6050 (แผ่นดินไหว) ต่างก็เรียก
     `Wire.begin(sda,scl)` บน object `Wire` ตัวเดียวกันคนละพิน ตอนแยกไฟล์ไม่มีปัญหา
     (รันคนละเฟส) แต่รวมไฟล์แล้วจะชนกัน (Wire ตัวเดียวกำหนดพินได้ชุดเดียว) — ESP32-S3
     มี I2C ฮาร์ดแวร์ 2 ชุด จึงแยกใช้ `Wire` สำหรับ MPU-6050 และ `Wire1` สำหรับ OLED

  3) **กล้องใช้ทรัพยากรร่วมกัน**: เดิม phase4 ใช้ EloquentEsp32cam (ความละเอียด
     240x240 สำหรับจดจำใบหน้า) ส่วน phase5d ใช้ esp_camera ตรงๆ ที่ SVGA (800x600)
     — กล้องตัวเดียวตั้งค่าได้ทีละความละเอียดเท่านั้น รวมไฟล์แล้วจึงใช้ความละเอียด
     240x240 (ของ EloquentEsp32cam) เป็นค่าเดียวสำหรับทั้งจดจำใบหน้าและตรวจจับการล้ม
     (ย่อลง 96x96 ด้วย box-filter เหมือนเดิม แค่จากภาพต้นทาง 240x240 แทน 800x600)

     **สำคัญ — ต้องทดสอบซ้ำ**: โมเดลตรวจจับการล้มเทรนจากภาพต้นทาง SVGA (800x600)
     สัดส่วนภาพ/มุมมองต่างจาก 240x240 (สี่เหลี่ยมจัตุรัส) พอสมควร ความแม่นยำอาจ
     ลดลงจากที่เคยวัดได้ (validation 97% ตอนนั้นอิงข้อมูลจาก SVGA) ต้องทดสอบจริงกับ
     ท่ายืน/นั่ง/ล้ม อีกครั้งหลังแฟลชเฟสนี้ ถ้าความแม่นยำตกมาก อาจต้องเก็บ dataset
     ใหม่ที่ 240x240 แล้วเทรนโมเดลใหม่

  4) **Buzzer ตัวเดียวของบ้าน**: รวมสถานะแจ้งเตือนทุกห้อง (ล้ม/SOS/แผ่นดินไหว/CO)
     เป็นตัวแปรเดียว ถ้าห้องไหนแจ้งเตือนอยู่ (active) buzzer จะดัง ยกเลิกทีละห้องจาก
     เว็บได้อิสระ (ห้องอื่นที่ยัง active buzzer จะดังต่อจนกว่าจะยกเลิกครบทุกห้อง)

  ขา: ดู include/unified_pins.h (รวมทุก *_pins.h เดิม + แก้จุดชนแล้ว)
  พาร์ทิชัน: ดู partitions_final.csv (ขยาย app0 เป็น 8MB เพราะรวมไลบรารีเยอะ)
  ต้องมี include/wifi_credentials.h ก่อน
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>

// esp32-camera (sensor.h) กับ Adafruit Unified Sensor ต่างก็มี typedef ชื่อ sensor_t
// ชนกันตรงๆ ถ้า include ทั้งคู่ในไฟล์เดียว — ครอบด้วย macro เปลี่ยนชื่อชั่วคราวตอน
// include ฝั่งกล้องเท่านั้น (เทคนิคมาตรฐานที่ใช้กันทั่วไปเวลารวมกล้อง ESP32 กับ
// ไลบรารี Adafruit sensor ในไฟล์เดียว)
#define sensor_t esp_camera_sensor_t
#include <esp_camera.h>
#include <img_converters.h>
#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>
#undef sensor_t

#include <ArduTFLite.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "unified_pins.h"
#include "fall_model_data.h"
#include "wifi_credentials.h"

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

// ===================== ค่าคงที่/เกณฑ์ (เอามาจากแต่ละเฟสเดิม ไม่เปลี่ยนตัวเลข) =====================

// -- ห้องโถง: ประตู/จดจำใบหน้า --
const int LOCK_ANGLE = 90;   // สลับกับ UNLOCK_ANGLE แล้ว (2026-08-02) เพราะ servo หมุนกลับด้าน
const int UNLOCK_ANGLE = 0;
const unsigned long DOOR_OPEN_MS = 4000;
const unsigned long RESCAN_COOLDOWN_MS = 8000;
const float FACE_CONFIDENCE = 0.87;  // ปรับหลังทดสอบจริง 2026-08-01: มุมกล้อง frontal ที่ดีที่สุดได้แค่ ~0.90 (ต้องใช้งานมุม frontal เท่านั้น)

// -- ตรวจจับการล้ม --
const char *FALL_LABELS[] = {"standing", "sitting", "fallen"};
const int NUM_FALL_LABELS = 3;
const int FALLEN_INDEX = 2;
const float FALL_CONFIDENCE_THRESHOLD = 0.7;
const unsigned long FALL_SUSTAINED_MS = 3000;  // ต้องเห็นว่า "ล้ม" ต่อเนื่องกี่ ms ถึงจะแจ้งเตือนจริง (กันทายพลาดเฟรมเดียว)
constexpr int TENSOR_ARENA_SIZE = 500 * 1024;
const int MODEL_SIZE = 96;
const int CAM_SIZE = 240;  // ความละเอียดกล้องร่วม (resolution.face())

// -- ห้องน้ำ: ปุ่ม SOS --
const unsigned long SOS_DEBOUNCE_MS = 50;

// -- แผ่นดินไหว --
const float GRAVITY_MS2 = 9.8f;
const float SHAKE_THRESHOLD_MS2 = 2.0f;
const int EQ_WINDOW_SIZE = 20;
const int EQ_WINDOW_TRIGGER_COUNT = 6;
const unsigned long EQ_SAMPLE_INTERVAL_MS = 50;

// -- ห้องครัว: MQ-7 + ปั๊มน้ำ --
const int CO_THRESHOLD_RAW = 2000;  // ปรับจริงจากการทดสอบหน้างาน (ดู phase9 เดิม)
const unsigned long CO_SUSTAINED_DURATION_MS = 3000;
const unsigned long PUMP_BURST_MS = 3000;
const unsigned long CO_SAMPLE_INTERVAL_MS = 200;

// -- ห้องนอน --
const unsigned long DHT_READ_INTERVAL_MS = 2000;
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_ADDR   0x3C

// ===================== อุปกรณ์ =====================
Servo doorServo;
DHT dht(DHT22_PIN, DHT22);
// OLED กับ MPU-6050 ใช้บัส Wire ร่วมกัน (กล้องจับจองบัสฮาร์ดแวร์ตัวที่ 2 ไปแล้ว
// ดูรายละเอียดที่คอมเมนต์ในฟังก์ชัน setup()) — ต้องต่อสาย SDA/SCL ของ OLED ขนาน
// ไปกับสาย MPU-6050 ที่ IO21/IO47 จริง ไม่ใช่ IO42/IO41 เดิม
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_MPU6050 mpu;

// ===================== mutex/สถานะรวม =====================
SemaphoreHandle_t cameraMutex;
SemaphoreHandle_t stateMutex;

bool cameraOk = false;  // false ถ้ากล้อง/จดจำใบหน้า init ไม่ผ่าน — ข้ามฟีเจอร์ห้องโถงใน loop()

byte *tensorArena = nullptr;
uint8_t *rgb888Buf = nullptr;

// -- หน้า/ประตู --
unsigned long lastUnlockAt = 0;
char lastFaceName[32] = "-";
float lastFaceSimilarity = 0;
unsigned long lastFaceAt = 0;
bool lastFaceUnlocked = false;

// -- ตรวจจับการล้ม --
// ปิดไว้เป็นค่าเริ่มต้น เพราะยังไม่แม่น (ต้องเก็บ dataset ใหม่ + เทรนใหม่) — ผู้ใช้
// กดเปิดเองจากเว็บตอนจะทดสอบ กันแจ้งเตือนเท็จตอนหันกล้องไปที่ไม่มีคน
bool fallDetectionEnabled = false;
bool fallActive = false;
unsigned long fallTimestampMs = 0;
float fallScores[NUM_FALL_LABELS] = {0, 0, 0};
char fallLabel[16] = "-";
unsigned long fallenSinceMs = 0;  // 0 = ยังไม่เห็นว่าล้มอยู่ตอนนี้
uint8_t *latestJpegBuf = nullptr;
size_t latestJpegLen = 0;

// -- ห้องนอน --
float bedroomTempC = NAN;
float bedroomHumidity = NAN;

// -- ห้องน้ำ --
bool sosActive = false;
unsigned long sosTimestampMs = 0;
int buttonIdleState = LOW;

// -- แผ่นดินไหว --
bool eqActive = false;
unsigned long eqTimestampMs = 0;
float eqPeakDelta = 0;
bool mpuFound = false;
bool shakeWindow[EQ_WINDOW_SIZE] = {false};
int eqWindowIndex = 0;
int shakeCountInWindow = 0;

// -- ห้องครัว --
bool coActive = false;
unsigned long coTimestampMs = 0;
int latestCoRaw = 0;
bool pumpOn = false;
unsigned long pumpStartMs = 0;
unsigned long aboveThresholdSinceMs = 0;
bool relayManualOn = false;  // ปุ่มทดสอบ relay มือ (แยกจาก pumpOn อัตโนมัติ ไว้เช็ค LED/สาย)

httpd_handle_t webHttpd = NULL;
httpd_handle_t streamHttpd = NULL;

void unlockDoor(const char *reason);

// buzzer เดียวของบ้าน: ดังถ้ามีห้องไหน active อยู่อย่างน้อย 1 ห้อง
// เรียกใน critical section ที่ถือ stateMutex อยู่แล้วเท่านั้น
void updateBuzzerLocked() {
  bool any = fallActive || sosActive || eqActive || coActive;
  digitalWrite(BUZZER_PIN, any ? HIGH : LOW);
}

// ===================== เว็บ dashboard =====================

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
<title>บ้านจำลองผู้สูงอายุ - Dashboard</title>
<style>
  * { box-sizing: border-box; }
  body {
    font-family: 'Segoe UI', sans-serif; background: #0b0f14; color: #e8eef4;
    margin: 0; padding: 20px 14px 60px;
  }
  h1 { text-align: center; font-size: 22px; font-weight: 600; margin: 4px 0 20px; letter-spacing: .3px; }
  .grid {
    display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 16px; max-width: 1100px; margin: 0 auto;
  }
  .card {
    background: #131a22; border-radius: 14px; padding: 18px 18px 16px;
    box-shadow: 0 2px 10px rgba(0,0,0,.35); border: 1px solid #1e2833;
    transition: background .25s, border-color .25s;
  }
  .card.alert { background: #4a1414; border-color: #b71c1c; }
  .card h2 { font-size: 16px; margin: 0 0 12px; display: flex; align-items: center; gap: 8px; }
  .badge {
    display: inline-block; font-size: 12px; padding: 3px 10px; border-radius: 20px;
    background: #1e2f22; color: #7ee08a; margin-left: auto;
  }
  .card.alert .badge { background: #7a1f1f; color: #ffb3b3; }
  .row { display: flex; justify-content: space-between; }
  .big { font-size: 30px; font-weight: 700; margin: 6px 0; }
  .sub { font-size: 13px; color: #9db0c0; }
  .card.alert .sub { color: #ffd9d9; }
  .bar-wrap { margin-top: 6px; font-size: 13px; }
  .bar-bg { background: #223; border-radius: 6px; height: 8px; margin: 3px 0 8px; overflow: hidden; }
  .bar-fill { background: #5aa9ff; height: 100%; }
  .bar-fill.fall { background: #ff5a5a; }
  button {
    margin-top: 12px; padding: 10px 18px; font-size: 14px; border: none; border-radius: 8px;
    background: #2e7d32; color: #fff; font-weight: 600; cursor: pointer; width: 100%;
  }
  button.cancel { background: #fff; color: #b71c1c; }
  button:active { filter: brightness(.9); }
  #stream { width: 100%; border-radius: 10px; margin-top: 8px; background: #000; image-rendering: pixelated; }
  .foot { text-align: center; margin-top: 22px; font-size: 12px; color: #64778a; }
</style>
</head>
<body>
  <h1>🏠 บ้านจำลองผู้สูงอายุ — ระบบตรวจสอบรวม</h1>
  <div class="grid">

    <div class="card" id="card-hall">
      <h2>🚪 ห้องโถง — ประตู/ใบหน้า <span class="badge" id="hall-badge">-</span></h2>
      <img id="stream" alt="กล้องสด">
      <div class="sub" id="hall-detail" style="margin-top:8px;">กำลังโหลด...</div>
      <button onclick="api('/open-door')">เปิดประตู (สำรอง)</button>
    </div>

    <div class="card" id="card-fall">
      <h2>🧍 ตรวจจับการล้ม <span class="badge" id="fall-badge">-</span></h2>
      <div class="bar-wrap">ยืน <div class="bar-bg"><div class="bar-fill" id="bar-standing" style="width:0%"></div></div></div>
      <div class="bar-wrap">นั่ง <div class="bar-bg"><div class="bar-fill" id="bar-sitting" style="width:0%"></div></div></div>
      <div class="bar-wrap">ล้ม <div class="bar-bg"><div class="bar-fill fall" id="bar-fallen" style="width:0%"></div></div></div>
      <button id="fall-toggle" onclick="toggleFall()">เปิดใช้งาน</button>
      <button class="cancel" id="fall-cancel" style="display:none" onclick="api('/cancel-fall')">รับทราบ / ยกเลิก</button>
    </div>

    <div class="card" id="card-bedroom">
      <h2>🛏️ ห้องนอน <span class="badge" id="bedroom-badge">-</span></h2>
      <div class="big" id="bedroom-temp">--°C</div>
      <div class="sub" id="bedroom-humidity">ความชื้น --%</div>
    </div>

    <div class="card" id="card-bathroom">
      <h2>🚿 ห้องน้ำ — ปุ่ม SOS <span class="badge" id="bathroom-badge">-</span></h2>
      <div class="sub" id="bathroom-detail">ปกติ ไม่มีการแจ้งเตือน</div>
      <button class="cancel" id="bathroom-cancel" style="display:none" onclick="api('/cancel-sos')">รับทราบ / ยกเลิก</button>
    </div>

    <div class="card" id="card-earthquake">
      <h2>🌍 แผ่นดินไหว <span class="badge" id="eq-badge">-</span></h2>
      <div class="sub" id="eq-detail">ปกติ ไม่มีการสั่นผิดปกติ</div>
      <button class="cancel" id="eq-cancel" style="display:none" onclick="api('/cancel-earthquake')">รับทราบ / ยกเลิก</button>
    </div>

    <div class="card" id="card-kitchen">
      <h2>🍳 ห้องครัว — CO <span class="badge" id="kitchen-badge">-</span></h2>
      <div class="sub" id="kitchen-detail">ปกติ ไม่มีการแจ้งเตือน</div>
      <button id="relay-toggle" onclick="toggleRelay()">ทดสอบ Relay (มือ)</button>
      <button class="cancel" id="kitchen-cancel" style="display:none" onclick="api('/cancel-co')">รับทราบ / ยกเลิก</button>
    </div>

  </div>
  <div class="foot">อัปเดตอัตโนมัติทุก 1.2 วินาที</div>

<script>
document.getElementById('stream').src = 'http://' + window.location.hostname + ':81/stream';

function api(path) { fetch(path).then(refresh); }

function toggleFall() {
  const btn = document.getElementById('fall-toggle');
  api(btn.dataset.enabled === '1' ? '/fall-off' : '/fall-on');
}

function setBadge(id, ok, okText, alertText) {
  const el = document.getElementById(id);
  el.innerText = ok ? okText : alertText;
}

function refresh() {
  fetch('/status').then(r => r.json()).then(s => {
    // hall / face
    document.getElementById('hall-badge').innerText = s.hall.unlocked ? 'ปลดล็อกแล้ว' : 'ล็อกอยู่';
    let hallText;
    if (s.hall.name === '-') hallText = 'ยังไม่เจอใบหน้า';
    else if (s.hall.name === 'unknown') hallText = 'เจอใบหน้าแต่จำไม่ได้ (' + s.hall.similarity.toFixed(2) + ')';
    else hallText = (s.hall.unlocked ? '✅ ปลดล็อกให้: ' : 'จำได้ว่าเป็น: ') + s.hall.name + ' (' + s.hall.similarity.toFixed(2) + ')';
    document.getElementById('hall-detail').innerText = hallText;

    // fall
    const fallCard = document.getElementById('card-fall');
    fallCard.classList.toggle('alert', s.fall.active);
    document.getElementById('fall-badge').innerText = s.fall.active ? 'ตรวจพบการล้ม!' : (s.fall.enabled ? s.fall.label : 'ปิดอยู่');
    document.getElementById('bar-standing').style.width = (s.fall.standing*100).toFixed(0) + '%';
    document.getElementById('bar-sitting').style.width = (s.fall.sitting*100).toFixed(0) + '%';
    document.getElementById('bar-fallen').style.width = (s.fall.fallen*100).toFixed(0) + '%';
    document.getElementById('fall-cancel').style.display = s.fall.active ? 'block' : 'none';
    const fallToggleBtn = document.getElementById('fall-toggle');
    fallToggleBtn.dataset.enabled = s.fall.enabled ? '1' : '0';
    fallToggleBtn.innerText = s.fall.enabled ? 'ปิดใช้งาน' : 'เปิดใช้งาน';
    fallToggleBtn.style.background = s.fall.enabled ? '#b71c1c' : '#2e7d32';

    // bedroom
    document.getElementById('bedroom-badge').innerText = s.bedroom.ok ? 'ปกติ' : 'ไม่พบเซนเซอร์';
    document.getElementById('bedroom-temp').innerText = s.bedroom.ok ? s.bedroom.tempC.toFixed(1) + '°C' : '--°C';
    document.getElementById('bedroom-humidity').innerText = 'ความชื้น ' + (s.bedroom.ok ? s.bedroom.humidity.toFixed(0) : '--') + '%';

    // bathroom
    const bathCard = document.getElementById('card-bathroom');
    bathCard.classList.toggle('alert', s.bathroom.active);
    document.getElementById('bathroom-badge').innerText = s.bathroom.active ? 'กด SOS!' : 'ปกติ';
    document.getElementById('bathroom-detail').innerText = s.bathroom.active
      ? '🚨 มีคนกดปุ่ม SOS! (' + Math.floor(s.bathroom.sinceMs/1000) + ' วินาทีที่แล้ว)' : 'ปกติ ไม่มีการแจ้งเตือน';
    document.getElementById('bathroom-cancel').style.display = s.bathroom.active ? 'block' : 'none';

    // earthquake
    const eqCard = document.getElementById('card-earthquake');
    eqCard.classList.toggle('alert', s.earthquake.active);
    document.getElementById('eq-badge').innerText = s.earthquake.active ? 'ตรวจพบการสั่น!' : (s.earthquake.mpuOk ? 'ปกติ' : 'ไม่พบเซนเซอร์');
    document.getElementById('eq-detail').innerText = s.earthquake.active
      ? '🚨 สั่นสะเทือนคล้ายแผ่นดินไหว! เบี่ยงเบนสูงสุด ' + s.earthquake.peakDelta.toFixed(2) + ' m/s²'
      : (s.earthquake.mpuOk ? 'ปกติ ไม่มีการสั่นผิดปกติ' : 'ไม่พบเซนเซอร์ MPU-6050');
    document.getElementById('eq-cancel').style.display = s.earthquake.active ? 'block' : 'none';

    // kitchen
    const kitCard = document.getElementById('card-kitchen');
    kitCard.classList.toggle('alert', s.kitchen.active);
    document.getElementById('kitchen-badge').innerText = s.kitchen.active ? 'CO สูง!' : 'ปกติ';
    document.getElementById('kitchen-detail').innerText =
      (s.kitchen.active ? '🚨 CO สูงต่อเนื่อง! ' : 'ปกติ ') + '(ค่าดิบ ' + s.kitchen.coRaw + ')' + (s.kitchen.pumpOn ? ' | ปั๊มน้ำ: กำลังฉีดน้ำ' : '');
    document.getElementById('kitchen-cancel').style.display = s.kitchen.active ? 'block' : 'none';
    const relayBtn = document.getElementById('relay-toggle');
    relayBtn.dataset.on = s.kitchen.relayManualOn ? '1' : '0';
    relayBtn.innerText = s.kitchen.relayManualOn ? 'ปิด Relay (มือ)' : 'ทดสอบ Relay (มือ)';
    relayBtn.style.background = s.kitchen.relayManualOn ? '#b71c1c' : '#2e7d32';
  }).catch(() => {});
}

function toggleRelay() {
  const btn = document.getElementById('relay-toggle');
  const turningOn = btn.dataset.on !== '1';
  btn.dataset.on = turningOn ? '1' : '0';
  api(turningOn ? '/relay-on' : '/relay-off');
}
setInterval(refresh, 1200);
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

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (latestJpegBuf != nullptr && latestJpegLen > 0) {
      jpegLen = latestJpegLen;
      jpegCopy = (uint8_t *)malloc(jpegLen);
      if (jpegCopy != NULL) memcpy(jpegCopy, latestJpegBuf, jpegLen);
    }
    xSemaphoreGive(stateMutex);

    if (jpegCopy == NULL) {
      delay(50);
      continue;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpegLen);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpegCopy, jpegLen);

    free(jpegCopy);
    if (res != ESP_OK) break;
    delay(80);
  }
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  char json[900];

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  unsigned long faceSecondsAgo = (millis() - lastFaceAt) / 1000;
  unsigned long sosSinceMs = sosActive ? (millis() - sosTimestampMs) : 0;
  unsigned long eqSinceMs = eqActive ? (millis() - eqTimestampMs) : 0;
  unsigned long coSinceMs = coActive ? (millis() - coTimestampMs) : 0;

  // DHT22 อ่านไม่ได้ (เซนเซอร์ไม่ได้ต่อ) คืนค่า NaN — ถ้าใส่ NaN ลง JSON ตรงๆ ด้วย
  // %.2f จะได้ข้อความ "nan" ซึ่งไม่ใช่ JSON ที่ถูกต้อง ทำให้ JSON.parse() ฝั่งเว็บ
  // ล้มเหลวทั้งก้อน (หน้าเว็บทั้งหน้าค้าง ไม่ใช่แค่ห้องนอน) ต้องแทนด้วย 0 ก่อนพิมพ์
  bool bedroomOk = !isnan(bedroomTempC) && !isnan(bedroomHumidity);
  float safeTempC = bedroomOk ? bedroomTempC : 0.0f;
  float safeHumidity = bedroomOk ? bedroomHumidity : 0.0f;

  snprintf(json, sizeof(json),
    "{"
    "\"hall\":{\"name\":\"%s\",\"similarity\":%.2f,\"unlocked\":%s,\"secondsAgo\":%lu},"
    "\"fall\":{\"enabled\":%s,\"active\":%s,\"standing\":%.3f,\"sitting\":%.3f,\"fallen\":%.3f,\"label\":\"%s\"},"
    "\"bedroom\":{\"ok\":%s,\"tempC\":%.2f,\"humidity\":%.2f},"
    "\"bathroom\":{\"active\":%s,\"sinceMs\":%lu},"
    "\"earthquake\":{\"active\":%s,\"sinceMs\":%lu,\"peakDelta\":%.2f,\"mpuOk\":%s},"
    "\"kitchen\":{\"active\":%s,\"sinceMs\":%lu,\"coRaw\":%d,\"pumpOn\":%s,\"relayManualOn\":%s}"
    "}",
    lastFaceName, lastFaceSimilarity, lastFaceUnlocked ? "true" : "false", faceSecondsAgo,
    fallDetectionEnabled ? "true" : "false", fallActive ? "true" : "false", fallScores[0], fallScores[1], fallScores[2], fallLabel,
    bedroomOk ? "true" : "false", safeTempC, safeHumidity,
    sosActive ? "true" : "false", sosSinceMs,
    eqActive ? "true" : "false", eqSinceMs, eqPeakDelta, mpuFound ? "true" : "false",
    coActive ? "true" : "false", coSinceMs, latestCoRaw, pumpOn ? "true" : "false", relayManualOn ? "true" : "false"
  );
  xSemaphoreGive(stateMutex);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t openDoorHandler(httpd_req_t *req) {
  unlockDoor("ปุ่มเว็บ (สำรอง)");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(req, "เปิดประตูแล้ว!");
}

static esp_err_t cancelFallHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  fallActive = false;
  updateBuzzerLocked();
  xSemaphoreGive(stateMutex);
  fallenSinceMs = 0;
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t fallOnHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  fallDetectionEnabled = true;
  xSemaphoreGive(stateMutex);
  fallenSinceMs = 0;  // เริ่มนับใหม่ กันสัญญาณเก่าตกค้างจากก่อนปิด
  Serial.println(">>> เปิดใช้งานตรวจจับการล้มจากเว็บแล้ว <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t fallOffHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  fallDetectionEnabled = false;
  fallActive = false;
  updateBuzzerLocked();
  xSemaphoreGive(stateMutex);
  fallenSinceMs = 0;
  Serial.println(">>> ปิดใช้งานตรวจจับการล้มจากเว็บแล้ว <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t cancelSosHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sosActive = false;
  updateBuzzerLocked();
  xSemaphoreGive(stateMutex);
  Serial.println(">>> ยกเลิกการแจ้งเตือน SOS จากเว็บแล้ว <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t cancelEarthquakeHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  eqActive = false;
  eqPeakDelta = 0;
  updateBuzzerLocked();
  xSemaphoreGive(stateMutex);
  for (int i = 0; i < EQ_WINDOW_SIZE; i++) shakeWindow[i] = false;
  shakeCountInWindow = 0;
  Serial.println(">>> ยกเลิกการแจ้งเตือนแผ่นดินไหวจากเว็บแล้ว <<<");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t cancelCoHandler(httpd_req_t *req) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  coActive = false;
  updateBuzzerLocked();
  xSemaphoreGive(stateMutex);
  aboveThresholdSinceMs = 0;
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
  config.ctrl_port = 32768;
  config.max_uri_handlers = 14;

  httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, NULL};
  httpd_uri_t statusUri = {"/status", HTTP_GET, statusHandler, NULL};
  httpd_uri_t openDoorUri = {"/open-door", HTTP_GET, openDoorHandler, NULL};
  httpd_uri_t cancelFallUri = {"/cancel-fall", HTTP_GET, cancelFallHandler, NULL};
  httpd_uri_t fallOnUri = {"/fall-on", HTTP_GET, fallOnHandler, NULL};
  httpd_uri_t fallOffUri = {"/fall-off", HTTP_GET, fallOffHandler, NULL};
  httpd_uri_t cancelSosUri = {"/cancel-sos", HTTP_GET, cancelSosHandler, NULL};
  httpd_uri_t cancelEqUri = {"/cancel-earthquake", HTTP_GET, cancelEarthquakeHandler, NULL};
  httpd_uri_t cancelCoUri = {"/cancel-co", HTTP_GET, cancelCoHandler, NULL};
  httpd_uri_t relayOnUri = {"/relay-on", HTTP_GET, relayOnHandler, NULL};
  httpd_uri_t relayOffUri = {"/relay-off", HTTP_GET, relayOffHandler, NULL};

  if (httpd_start(&webHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(webHttpd, &indexUri);
    httpd_register_uri_handler(webHttpd, &statusUri);
    httpd_register_uri_handler(webHttpd, &openDoorUri);
    httpd_register_uri_handler(webHttpd, &cancelFallUri);
    httpd_register_uri_handler(webHttpd, &fallOnUri);
    httpd_register_uri_handler(webHttpd, &fallOffUri);
    httpd_register_uri_handler(webHttpd, &cancelSosUri);
    httpd_register_uri_handler(webHttpd, &cancelEqUri);
    httpd_register_uri_handler(webHttpd, &cancelCoUri);
    httpd_register_uri_handler(webHttpd, &relayOnUri);
    httpd_register_uri_handler(webHttpd, &relayOffUri);
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, NULL};
  if (httpd_start(&streamHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
  }
}

// ===================== ห้องโถง: ประตู/ใบหน้า =====================

void unlockDoor(const char *reason) {
  Serial.printf(">>> ปลดล็อกประตู (%s) <<<\n", reason);
  doorServo.write(UNLOCK_ANGLE);
  delay(DOOR_OPEN_MS);
  doorServo.write(LOCK_ANGLE);
  Serial.println(">>> ล็อกประตูกลับแล้ว <<<");
  lastUnlockAt = millis();
}

// ===================== ตรวจจับการล้ม =====================
// ย่อภาพ 240x240 (แหล่งเดียวกับที่ใช้จดจำใบหน้า) เป็น 96x96 ด้วย box-filter
//
// แก้ไข (2026-08-01 หลังทดสอบจริงพบทายผิดบ่อย — ยืน/ไม่มีคนในเฟรม ถูกทายว่าล้ม):
// ตอนเทรนโมเดลใช้ภาพต้นทาง 800x600 (สัดส่วน 4:3 แนวนอน) บีบลง 96x96 ตรงๆ แต่
// ตอนนี้กล้องต้องใช้โหมด 240x240 (สี่เหลี่ยมจัตุรัส 1:1) ร่วมกับจดจำใบหน้า ถ้าบีบ
// เต็มกรอบสี่เหลี่ยมจัตุรัสลง 96x96 ตรงๆ สัดส่วน/มุมมองภาพจะต่างจากตอนเทรนมาก
// เกินไป จึงตัดขอบบน-ล่างออกก่อน ให้เหลือกรอบสัดส่วน 4:3 เหมือนตอนเทรน (แล้วค่อย
// บีบลง 96x96) ไม่ได้แก้ปัญหาความละเอียด/มุมมองกล้องที่ต่างกันทั้งหมด (ยังต้องทดสอบ
// จริงว่าดีขึ้นแค่ไหน) ถ้ายังไม่แม่นพอ ต้องเก็บ dataset ใหม่ที่ 240x240 แล้วเทรนใหม่
bool feedFrameToFallModel(camera_fb_t *fb) {
  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb888Buf)) {
    return false;
  }

  int fullW = fb->width;
  int fullH = fb->height;

  int srcW = fullW;
  int srcH = fullW * 3 / 4;  // ตัดให้ได้สัดส่วน 4:3 (กว้าง:สูง) เหมือนตอนเทรน
  if (srcH > fullH) srcH = fullH;
  int cropTop = (fullH - srcH) / 2;  // ตัดขอบบน-ล่างเท่ากัน เอากึ่งกลางเฟรมไว้

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
          int srcIdx = ((cropTop + sy) * fullW + sx) * 3;
          sumR += rgb888Buf[srcIdx + 0];
          sumG += rgb888Buf[srcIdx + 1];
          sumB += rgb888Buf[srcIdx + 2];
          count++;
        }
      }
      uint8_t avgR = sumR / count, avgG = sumG / count, avgB = sumB / count;

      // ห้ามหาร 255 ตรงนี้ — โมเดลมี Rescaling(1/255) เป็นเลเยอร์แรกในตัวอยู่แล้ว
      int dstIdx = (oy * MODEL_SIZE + ox) * 3;
      modelSetInput((float)avgR, dstIdx + 0);
      modelSetInput((float)avgG, dstIdx + 1);
      modelSetInput((float)avgB, dstIdx + 2);
    }
  }
  return true;
}

void handleFallDetection(camera_fb_t *fb) {
  bool ok = feedFrameToFallModel(fb);
  if (!ok || !modelRunInference()) return;

  float scores[NUM_FALL_LABELS];
  int bestIndex = 0;
  for (int i = 0; i < NUM_FALL_LABELS; i++) {
    scores[i] = modelGetOutput(i);
    if (scores[i] > scores[bestIndex]) bestIndex = i;
  }

  static unsigned long lastDebugMs = 0;
  if (millis() - lastDebugMs > 500) {
    lastDebugMs = millis();
    Serial.printf("[fall-debug] ยืน(idx0)=%.2f นั่ง(idx1)=%.2f ล้ม(idx2)=%.2f -> %s\n",
                  scores[0], scores[1], scores[2], FALL_LABELS[bestIndex]);
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < NUM_FALL_LABELS; i++) fallScores[i] = scores[i];
  strncpy(fallLabel, FALL_LABELS[bestIndex], sizeof(fallLabel) - 1);
  fallLabel[sizeof(fallLabel) - 1] = '\0';
  bool wasActive = fallActive;
  xSemaphoreGive(stateMutex);

  unsigned long now = millis();
  if (bestIndex == FALLEN_INDEX && scores[FALLEN_INDEX] >= FALL_CONFIDENCE_THRESHOLD) {
    if (fallenSinceMs == 0) fallenSinceMs = now;  // เพิ่งเริ่มเห็นว่าล้ม
    if (!wasActive && now - fallenSinceMs >= FALL_SUSTAINED_MS) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      fallActive = true;
      fallTimestampMs = now;
      updateBuzzerLocked();
      xSemaphoreGive(stateMutex);
      Serial.printf(">>> ตรวจพบการล้มต่อเนื่อง %lu ms! (confidence %.2f) — Buzzer ดัง <<<\n",
                    FALL_SUSTAINED_MS, scores[FALLEN_INDEX]);
    }
  } else {
    fallenSinceMs = 0;  // หลุดจากท่าล้มแล้ว รีเซ็ตตัวจับเวลา กันสัญญาณเท็จเฟรมเดียว
  }
}

// ===================== จอ OLED: สถานะรวมทุกห้อง =====================
// จอ 128x64 แบ่งเป็น 7 แถว แถวละ 9px แสดงสถานะทุกห้องพร้อมกัน แถวไหนกำลัง
// แจ้งเตือนอยู่จะขึ้นพื้นขาวตัวอักษรดำ (invert) ให้เด่นชัดเห็นง่ายจากระยะไกล
void drawStatusLine(int y, const char *label, const char *value, bool alert) {
  if (alert) {
    display.fillRect(0, y, OLED_WIDTH, 9, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
  } else {
    display.setTextColor(SH110X_WHITE);
  }
  display.setCursor(0, y + 1);
  display.print(label);
  display.print(value);
}

void updateStatusDisplay(float tempC, float humidity, bool dhtOk,
                          bool hallUnlocked, bool fallOn, bool sosOn,
                          bool eqOn, bool coOn, int coRaw, bool pumpRunning) {
  display.clearDisplay();
  display.setTextSize(1);

  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  if (dhtOk) display.printf("T:%.1fC H:%.0f%%", tempC, humidity);
  else display.print("Bedroom: no sensor");

  char kitchenVal[20];
  snprintf(kitchenVal, sizeof(kitchenVal), coOn ? "CO HIGH! %d" : "ok (%d)", coRaw);

  drawStatusLine(9,  "Hall: ",   hallUnlocked ? "UNLOCKED" : "locked", false);
  drawStatusLine(18, "Fall: ",   fallOn ? "FALLEN!" : "ok", fallOn);
  drawStatusLine(27, "Bath: ",   sosOn ? "SOS!" : "ok", sosOn);
  drawStatusLine(36, "Quake: ",  eqOn ? "ALERT!" : "ok", eqOn);
  drawStatusLine(45, "Kitchen:", kitchenVal, coOn);
  drawStatusLine(54, "Pump: ",   pumpRunning ? "RUNNING" : "off", pumpRunning);

  display.display();
}

void handleBedroom() {
  static unsigned long lastReadMs = 0;
  if (millis() - lastReadMs < DHT_READ_INTERVAL_MS) return;
  lastReadMs = millis();

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  bool dhtOk = !isnan(h) && !isnan(t);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (dhtOk) {
    bedroomTempC = t;
    bedroomHumidity = h;
  }
  bool hallUnlocked = lastFaceUnlocked;
  bool fallOn = fallActive;
  bool sosOn = sosActive;
  bool eqOn = eqActive;
  bool coOn = coActive;
  int coRaw = latestCoRaw;
  bool pumpRunning = pumpOn;
  xSemaphoreGive(stateMutex);

  updateStatusDisplay(t, h, dhtOk, hallUnlocked, fallOn, sosOn, eqOn, coOn, coRaw, pumpRunning);
}

// ===================== ห้องน้ำ: ปุ่ม SOS =====================

void handleBathroom() {
  static bool lastRawPressed = false;
  static bool confirmedPressed = false;
  static unsigned long lastDebounceMs = 0;

  int rawState = digitalRead(SOS_BUTTON_PIN);
  bool rawPressed = (rawState != buttonIdleState);

  if (rawPressed != lastRawPressed) lastDebounceMs = millis();

  if (millis() - lastDebounceMs > SOS_DEBOUNCE_MS && rawPressed != confirmedPressed) {
    confirmedPressed = rawPressed;
    if (confirmedPressed) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      sosActive = true;
      sosTimestampMs = millis();
      updateBuzzerLocked();
      xSemaphoreGive(stateMutex);
      Serial.println(">>> กดปุ่ม SOS! Buzzer ดังทันที <<<");
    }
  }
  lastRawPressed = rawPressed;
}

// ===================== ห้องโถง: แผ่นดินไหว (MPU-6050) =====================

void handleEarthquake() {
  static unsigned long lastSampleMs = 0;
  if (!mpuFound || millis() - lastSampleMs < EQ_SAMPLE_INTERVAL_MS) return;
  lastSampleMs = millis();

  sensors_event_t a, g, temp;
  bool readOk = mpu.getEvent(&a, &g, &temp);

  float magnitude = sqrtf(a.acceleration.x * a.acceleration.x +
                           a.acceleration.y * a.acceleration.y +
                           a.acceleration.z * a.acceleration.z);

  if (!readOk || magnitude < 1.0f) return;

  float delta = fabsf(magnitude - GRAVITY_MS2);
  bool shaking = delta > SHAKE_THRESHOLD_MS2;

  if (shakeWindow[eqWindowIndex] != shaking) {
    shakeCountInWindow += shaking ? 1 : -1;
    shakeWindow[eqWindowIndex] = shaking;
  }
  eqWindowIndex = (eqWindowIndex + 1) % EQ_WINDOW_SIZE;

  bool wasActive;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  wasActive = eqActive;
  if (shaking && delta > eqPeakDelta) eqPeakDelta = delta;
  xSemaphoreGive(stateMutex);

  if (!wasActive && shakeCountInWindow >= EQ_WINDOW_TRIGGER_COUNT) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    eqActive = true;
    eqTimestampMs = millis();
    updateBuzzerLocked();
    xSemaphoreGive(stateMutex);
    Serial.printf(">>> ตรวจพบแรงสั่นสะเทือนคล้ายแผ่นดินไหว! (เบี่ยงเบนล่าสุด %.2f m/s^2) <<<\n", delta);
    unlockDoor("เหตุฉุกเฉิน: แผ่นดินไหว");  // เปิดประตูอัตโนมัติให้หนีออกได้
  }
}

// ===================== ห้องครัว: MQ-7 + ปั๊มน้ำ =====================

void handleKitchen() {
  static unsigned long lastSampleMs = 0;
  unsigned long now = millis();

  if (pumpOn && now - pumpStartMs >= PUMP_BURST_MS) {
    pumpOn = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println(">>> ปั๊มน้ำหยุดทำงาน (ครบเวลาฉีด) <<<");
  }

  if (now - lastSampleMs < CO_SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  int coRaw = analogRead(MQ7_PIN);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  latestCoRaw = coRaw;
  bool wasActive = coActive;
  xSemaphoreGive(stateMutex);

  bool overThreshold = coRaw > CO_THRESHOLD_RAW;

  if (overThreshold) {
    if (aboveThresholdSinceMs == 0) {
      aboveThresholdSinceMs = now;
    } else if (!wasActive && now - aboveThresholdSinceMs >= CO_SUSTAINED_DURATION_MS) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      coActive = true;
      coTimestampMs = now;
      pumpOn = true;
      pumpStartMs = now;
      updateBuzzerLocked();
      xSemaphoreGive(stateMutex);
      digitalWrite(RELAY_PIN, HIGH);
      Serial.printf(">>> ตรวจพบ CO สูงต่อเนื่อง! (ค่าดิบ %d) สั่งปั๊มน้ำ + Buzzer ดังทันที <<<\n", coRaw);
      unlockDoor("เหตุฉุกเฉิน: CO สูง/ไฟไหม้");  // เปิดประตูอัตโนมัติให้หนีออกได้
    }
  } else {
    aboveThresholdSinceMs = 0;
  }
}

// ===================== setup / loop =====================

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== บ้านจำลองผู้สูงอายุ: เฟิร์มแวร์รวมสุดท้าย =====");

  cameraMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();

  // ---- ตั้งค่าพิน output ----
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(SOS_BUTTON_PIN, INPUT);
  delay(100);
  buttonIdleState = digitalRead(SOS_BUTTON_PIN);

  // ---- กล้อง + จดจำใบหน้า (EloquentEsp32cam) ----
  camera.pinout.wroom_s3();
  camera.brownout.disable();
  camera.resolution.face();  // 240x240 — ใช้ร่วมกันทั้งจดจำใบหน้าและตรวจจับการล้ม
  camera.quality.high();

  detection.accurate();
  detection.confidence(0.7);
  recognition.confidence(FACE_CONFIDENCE);

  // กล้องล้มเหลวได้จากสาเหตุฮาร์ดแวร์ล้วนๆ (สาย/ต่อผิด) — ไม่ block รอตลอดไปแบบ
  // เฟสเดี่ยวเดิม (phase4) เพราะตอนนี้รวมทุกห้องในไฟล์เดียวแล้ว ถ้ากล้องเสีย
  // ห้องอื่น (นอน/น้ำ/แผ่นดินไหว/ครัว) ต้องยังทำงานได้ต่อ ไม่ควรถูกดึงตายไปด้วย
  // ลองไม่เกิน 5 ครั้งแล้วปล่อยผ่าน ถ้าไม่สำเร็จ ปิดเฉพาะฟีเจอร์ที่ต้องพึ่งกล้อง (ตัวแปร
  // cameraOk เป็น global ด้านบน ใช้เช็คใน loop() ด้วยว่าจะข้ามบล็อกกล้องทั้งหมดไหม)
  for (int attempt = 0; attempt < 5 && !cameraOk; attempt++) {
    if (camera.begin().isOk()) {
      cameraOk = true;
    } else {
      Serial.println(camera.exception.toString());
      delay(500);
    }
  }

  if (cameraOk && !recognition.begin().isOk()) {
    Serial.println(recognition.exception.toString());
    cameraOk = false;
  }

  if (cameraOk) {
    Serial.println(">>> CAMERA + FACE RECOGNIZER OK <<<");
  } else {
    Serial.println("!!! กล้อง/จดจำใบหน้าใช้งานไม่ได้ — ข้ามฟีเจอร์ห้องโถง (ประตู/ตรวจจับการล้ม) "
                    "ห้องอื่นยังทำงานต่อปกติ !!!");
  }

  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_ANGLE);
  Serial.printf("Servo attach ที่ GPIO%d แล้ว (ล็อกอยู่)\n", SERVO_PIN);

  // ---- บัฟเฟอร์สำหรับตรวจจับการล้ม (จองจาก PSRAM) — ข้ามถ้ากล้องใช้ไม่ได้อยู่แล้ว ----
  if (cameraOk) {
  rgb888Buf = (uint8_t *)heap_caps_malloc(CAM_SIZE * CAM_SIZE * 3, MALLOC_CAP_SPIRAM);
  if (rgb888Buf == nullptr) {
    Serial.println("!!! จอง rgb888Buf ไม่สำเร็จ (PSRAM ไม่พอ) !!!");
  }
  tensorArena = (byte *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM);
  if (tensorArena == nullptr) {
    Serial.println("!!! จอง tensor arena ไม่สำเร็จ (PSRAM ไม่พอ) !!!");
  }
  Serial.println("กำลังโหลดโมเดลตรวจจับการล้ม...");
  if (!modelInit(fall_model_data, tensorArena, TENSOR_ARENA_SIZE)) {
    Serial.println("!!! โหลดโมเดลไม่สำเร็จ !!!");
  } else {
    Serial.println(">>> โหลดโมเดลสำเร็จ <<<");
  }
  }  // end if (cameraOk) — บล็อกโหลดโมเดลตรวจจับการล้ม

  // ---- I2C บัสร่วม: MPU-6050 + OLED (แก้ไขหลังทดสอบจริง 2026-08-01) ----
  // เดิมตั้งใจแยก MPU-6050 กับ OLED คนละบัสฮาร์ดแวร์ (Wire/Wire1) เพราะกลัวชนกัน
  // แต่ทดสอบจริงพบว่ากล้อง (SCCB) ไปจับจองบัสฮาร์ดแวร์ตัวที่ 2 (ตรงกับ Wire1) เอง
  // ภายในตอน camera.begin() (สังเกตจาก error "i2c_driver_install failed" เฉพาะฝั่ง
  // OLED เท่านั้น ส่วน MPU-6050 บน Wire ปกติดี) เหลือฮาร์ดแวร์ I2C ว่างแค่บัสเดียว
  // (Wire) จึงต้องให้ MPU-6050 กับ OLED "ใช้บัสเดียวกันจริง" ตามหลัก I2C ปกติ (อุปกรณ์
  // หลายตัวแชร์บัสเดียวกันได้ด้วยที่อยู่ต่างกัน — MPU-6050 คือ 0x68, OLED คือ 0x3C
  // ไม่ชนกัน) **ต้องย้ายสาย OLED SDA/SCL จาก IO42/41 เดิม ไปต่อพ่วงร่วมกับสาย
  // MPU-6050 ที่ IO21(SDA)/IO47(SCL) แทน** (ต่อขนานกัน ไม่ใช่ย้ายออกจาก MPU-6050)
  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
  Wire.setClock(100000);

  dht.begin();
  // ลองทั้ง 0x3C และ 0x3D เพราะโมดูล SH1106 บางล็อตใช้ address ต่างกัน ไม่เดา
  // เอาเองว่าตัวไหนถูก ให้โค้ดลองจริงแล้วบันทึกไว้ว่าตัวไหนติด
  if (display.begin(OLED_ADDR, true)) {
    Serial.println(">>> OLED OK (address 0x3C) <<<");
    display.clearDisplay();
    display.display();
  } else if (display.begin(0x3D, true)) {
    Serial.println(">>> OLED OK (address 0x3D) <<<");
    display.clearDisplay();
    display.display();
  } else {
    Serial.println("!!! OLED INIT FAILED ที่ทั้ง 0x3C และ 0x3D !!!");
  }

  // ---- แผ่นดินไหว: MPU-6050 (บัสเดียวกับ OLED ด้านบน) ----
  if (!mpu.begin()) {
    Serial.println("!!! MPU-6050 INIT FAILED !!!");
  } else {
    Serial.println(">>> MPU-6050 INIT OK <<<");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpuFound = true;
  }

  // ---- WiFi + เว็บ ----
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
    Serial.println("!!! WiFi ต่อไม่ติด — เซนเซอร์/buzzer/ปั๊มน้ำยังทำงานได้ปกติ (ไม่พึ่งเครือข่าย) !!!");
  }

  Serial.println("===== เริ่มทำงานทุกระบบ =====");
}

void loop() {
  delay(15);  // กันไม่ให้ loop() แย่งคิว cameraMutex ถี่จน streamHandler() ไม่ได้เฟรมไปส่ง

  // ---- ห้องอื่นๆ ที่ไม่ต้องใช้กล้อง (เบา รันได้ทุกรอบ) ----
  handleBedroom();
  handleBathroom();
  handleEarthquake();
  handleKitchen();

  // ---- ห้องโถง: กล้อง + จดจำใบหน้า + ตรวจจับการล้ม (ใช้กล้องร่วมกัน) ----
  // ข้ามบล็อกนี้ทั้งหมดถ้ากล้อง init ไม่ผ่านตอน setup() — ห้องอื่นทำงานต่อได้ปกติ
  if (!cameraOk) return;

  xSemaphoreTake(cameraMutex, portMAX_DELAY);
  bool captured = camera.capture().isOk();
  if (captured) {
    // เก็บสำเนา JPEG ล่าสุดไว้ส่งขึ้นเว็บ /stream
    uint8_t *jpegCopy = (uint8_t *)malloc(camera.frame->len);
    if (jpegCopy != nullptr) {
      memcpy(jpegCopy, camera.frame->buf, camera.frame->len);
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      if (latestJpegBuf != nullptr) free(latestJpegBuf);
      latestJpegBuf = jpegCopy;
      latestJpegLen = camera.frame->len;
      xSemaphoreGive(stateMutex);
    }

    // จดจำใบหน้า + ปลดล็อกประตู
    bool detectOk = recognition.detect().isOk();
    bool faceFound = detectOk;

    static unsigned long lastFaceDebugMs = 0;
    if (millis() - lastFaceDebugMs > 1000) {
      lastFaceDebugMs = millis();
      Serial.printf("[face-debug] captured=1 detectOk=%d exception=%s\n",
                    detectOk, detectOk ? "-" : detection.exception.toString().c_str());
    }

    if (faceFound && millis() - lastUnlockAt >= RESCAN_COOLDOWN_MS) {
      if (recognition.recognize().isOk()) {
        bool isKnownPerson = recognition.match.name != "unknown" &&
                              recognition.match.similarity >= FACE_CONFIDENCE;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        strncpy(lastFaceName, recognition.match.name.c_str(), sizeof(lastFaceName) - 1);
        lastFaceName[sizeof(lastFaceName) - 1] = '\0';
        lastFaceSimilarity = recognition.match.similarity;
        lastFaceAt = millis();
        lastFaceUnlocked = isKnownPerson;
        xSemaphoreGive(stateMutex);

        if (isKnownPerson) unlockDoor(recognition.match.name.c_str());
      }
    }

    // ตรวจจับการล้ม (ใช้เฟรมเดียวกัน ไม่ต้องขอกล้องซ้ำ) — เฉพาะตอนเปิดใช้งานจากเว็บ
    if (fallDetectionEnabled && tensorArena != nullptr && rgb888Buf != nullptr) {
      handleFallDetection(camera.frame);
    }
  }
  xSemaphoreGive(cameraMutex);
}
