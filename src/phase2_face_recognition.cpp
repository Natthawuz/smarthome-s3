/*
  phase2_face_recognition.cpp
  -----------------------------------------------------------
  ทดสอบระบบจดจำใบหน้าสำหรับห้องโถง (ใช้ไลบรารี EloquentEsp32cam)
  ทำงานบนบอร์ด ESP32-S3 เท่านั้น (ตรงกับบอร์ดที่ใช้จริง)

  ที่มา: ปรับจากตัวอย่าง Face_Recognition.ino ที่ยืนยันว่าใช้งานได้จริง
  จาก repo ต้นทาง https://github.com/eloquentarduino/EloquentEsp32cam
  (เพิ่มคอมเมนต์/ข้อความเป็นภาษาไทย ตรรกะหลักเหมือนต้นฉบับ)

  จำหน้าได้ (recognize สำเร็จ ผ่านเกณฑ์ความมั่นใจ) -> สั่ง servo หมุนไปตำแหน่ง
  "ปลดล็อก" ค้างไว้ DOOR_OPEN_MS แล้วหมุนกลับตำแหน่ง "ล็อก" อัตโนมัติ
  (พิน servo คือ SERVO_PIN ในไฟล์ hall_pins.h ที่ยืนยันแล้วใน Phase 2 ห้องโถง)

  วิธีใช้ (ผ่าน Serial Monitor 115200):
  1. ตอนเปิดเครื่อง จะถามว่าจะลบใบหน้าที่เคยบันทึกไว้ทั้งหมดไหม / จะแสดงรายชื่อที่บันทึกไว้ไหม
  2. เอาหน้าเข้ากล้อง รอจนเจอใบหน้า จะถามว่า "enroll" (บันทึกใหม่) หรือ "recognize" (จำแนก)
     - พิมพ์ e แล้ว Enter เพื่อบันทึกใบหน้าใหม่ ใส่ชื่อครั้งเดียว แล้วใส่จำนวนรูปที่จะเทรน
       (แนะนำ 10-15 รูป) ระบบจะถามให้ขยับมุมหน้า/แสง/ระยะห่างเล็กน้อยแล้วกด Enter ถ่ายทีละรูป
       จนครบจำนวน — ยิ่งรูปเยอะ/มุมหลากหลาย ยิ่งแม่นและปัดคนแปลกหน้าได้เด็ดขาดขึ้น
     - พิมพ์ r แล้ว Enter เพื่อให้ระบบลองจำแนกว่าใบหน้านี้คือใคร ถ้าจำได้ servo จะปลดล็อกให้อัตโนมัติ
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <ESP32Servo.h>
#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>
#include "hall_pins.h"

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

const int LOCK_ANGLE = 0;      // มุม servo ตอน "ล็อกประตู"
const int UNLOCK_ANGLE = 90;   // มุม servo ตอน "ปลดล็อกประตู"
const unsigned long DOOR_OPEN_MS = 4000;  // ค้างปลดล็อกไว้กี่ ms ก่อนล็อกกลับ

Servo doorServo;

String prompt(String message);
void enroll();
void recognize();
void unlockDoor();

void setup() {
  delay(4000);
  Serial.begin(115200);
  Serial.println();
  Serial.println("===== ทดสอบจดจำใบหน้า (ห้องโถง) =====");

  // wroom_s3() พินตรงกับ camera_pins.h ที่ยืนยันแล้วใน Phase 0 ทุกตัว (d0-d7, xclk, pclk,
  // vsync, href, sccb_sda/scl, pwdn/reset) จึงใช้ preset นี้ได้เลยโดยไม่ต้องกำหนดพินเอง
  camera.pinout.wroom_s3();
  camera.brownout.disable();

  // การจดจำใบหน้าทำงานได้เฉพาะที่ความละเอียด 240x240 เท่านั้น (ข้อจำกัดของไลบรารี)
  camera.resolution.face();
  camera.quality.high();

  // การจดจำใบหน้าต้องใช้โหมด detection แบบ accurate (มี keypoints) เท่านั้น
  detection.accurate();
  detection.confidence(0.7);

  // ค่าความมั่นใจขั้นต่ำก่อนจะถือว่า "จำได้" ว่าเป็นคนที่เคย enroll ไว้
  // เพิ่มจาก 0.85 เป็น 0.93 หลังพบว่า 0.85 หลวมเกินไป — คนที่ไม่เคย enroll เลย
  // ถูกจำผิดเป็นคนที่ enroll ไว้แล้ว (false positive)
  // ลองเพิ่มเป็น 0.95 (2026-08-01) แต่ทดสอบจริงพบว่าหน้าคนที่ enroll เองสแกนได้แค่
  // ~0.93 บางครั้ง (แกว่งตามแสง/มุม/ระยะห่าง) ทำให้ถูกปัดเป็น unknown ทั้งที่เป็นคนจริง
  // ทดสอบเพิ่มเติมพบว่าแม้มุมกล้อง frontal ที่ดีที่สุด ก็ยังได้แค่ ~0.90 (ยิ่งมุมเอียง
  // ยิ่งลดลง 0.90->0.63 ตามมุมที่เบี้ยวออก) เกณฑ์ 0.93 จึงสูงเกินจริงสำหรับข้อมูลชุดนี้
  // ปรับลงเป็น 0.87 (ต่ำกว่าค่าดีที่สุดที่วัดได้จริงเล็กน้อย เผื่อความแกว่งปกติ) — ต้อง
  // ใช้งาน/ทดสอบด้วยมุมกล้องแบบ frontal (ยืนตรงหน้ากล้อง) เท่านั้นถึงจะแม่นยำดี
  recognition.confidence(0.87);

  while (!camera.begin().isOk())
    Serial.println(camera.exception.toString());

  while (!recognition.begin().isOk())
    Serial.println(recognition.exception.toString());

  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_ANGLE);
  Serial.printf("Servo attach ที่ GPIO%d แล้ว (ล็อกอยู่)\n", SERVO_PIN);

  Serial.println(">>> CAMERA OK <<<");
  Serial.println(">>> FACE RECOGNIZER OK <<<");

  if (prompt("ต้องการลบใบหน้าที่เคยบันทึกไว้ทั้งหมดหรือไม่? [yes|no]").startsWith("y")) {
    Serial.println("กำลังลบข้อมูลใบหน้าเดิมทั้งหมด...");
    recognition.deleteAll();
  }

  if (prompt("ต้องการแสดงรายชื่อใบหน้าที่บันทึกไว้หรือไม่? [yes|no]").startsWith("y")) {
    recognition.dump();
  }

  Serial.println("รอใบหน้า... เอาหน้าเข้ากล้องได้เลย");
}

void loop() {
  if (!camera.capture().isOk()) {
    Serial.println(camera.exception.toString());
    return;
  }

  // ตรวจแค่ว่ามีใบหน้าอยู่ในเฟรมหรือไม่ (ยังไม่จำแนกว่าเป็นใคร)
  if (!recognition.detect().isOk())
    return;

  String answer = prompt("เจอใบหน้าแล้ว ต้องการ enroll (บันทึกใหม่) หรือ recognize (จำแนก)? [e|r]");

  if (answer.startsWith("e"))
    enroll();
  else if (answer.startsWith("r"))
    recognize();

  Serial.println("รอใบหน้า...");
}

String prompt(String message) {
  String answer;

  do {
    Serial.print(message);
    Serial.print(" ");

    while (!Serial.available())
      delay(1);

    answer = Serial.readStringUntil('\n');
    answer.trim();
  } while (!answer.length());

  Serial.println(answer);
  return answer;
}

// Enroll แบบหลายรูปติดกันโดยไม่ต้องพิมพ์ชื่อซ้ำทุกรอบ — ไว้เทรนคนเดียวด้วยรูปจำนวนมาก
// (แนะนำ 10-15 รูป มุมหน้า/แสง/ระยะห่างต่างกันเล็กน้อยทุกรูป) ให้ความมั่นใจแม่นขึ้น
// และแยกแยะคนแปลกหน้าได้เด็ดขาดขึ้น (ดูคอมเมนต์ recognition.confidence() ด้านบน)
void enroll() {
  String name = prompt("ใส่ชื่อคนที่จะบันทึก:");
  int count = prompt("ต้องการบันทึกกี่รูป? (แนะนำ 10-15 รูป มุมหน้า/แสงต่างกันเล็กน้อยทุกรูป)").toInt();
  if (count <= 0) count = 1;

  int saved = 0;
  for (int i = 1; i <= count; i++) {
    if (i > 1) {
      // ใช้ prompt() ไม่ได้ตรงนี้ เพราะ prompt() บังคับว่าต้องได้คำตอบไม่ว่างเปล่า
      // (วนถามซ้ำถ้า answer ว่าง) แต่ที่นี่แค่ต้องการ "กด Enter เฉยๆ" ซึ่งส่งบรรทัดว่าง
      // มา ทำให้ prompt() ค้างวนถามซ้ำไม่รู้จบ — รอรับ Enter ตรงๆ แทน
      Serial.println("รูปที่ " + String(i) + "/" + String(count) + " — ขยับมุมหน้า/ระยะห่างเล็กน้อยแล้วกด Enter เพื่อถ่าย");
      while (!Serial.available())
        delay(1);
      Serial.readStringUntil('\n');

      if (!camera.capture().isOk()) {
        Serial.println(camera.exception.toString());
        i--;
        continue;
      }
      // ไม่เรียก recognition.detect() ซ้ำตรงนี้ — enroll() เช็คหน้าในเฟรมเองอยู่แล้ว
      // (เรียก detect() ซ้ำก่อนหน้านี้ทำให้ค้าง ไม่ผ่านทุกรอบตั้งแต่รูปที่ 2 เป็นต้นไป)
    }

    if (recognition.enroll(name).isOk()) {
      Serial.printf(">>> บันทึกรูปที่ %d/%d สำเร็จ (%s) <<<\n", i, count, name.c_str());
      saved++;
    } else {
      Serial.println(recognition.exception.toString());
      i--;
    }
  }

  Serial.printf(">>> เสร็จสิ้น: บันทึกสำเร็จ %d/%d รูป สำหรับ \"%s\" <<<\n", saved, count, name.c_str());
}

void recognize() {
  if (!recognition.recognize().isOk()) {
    Serial.println(recognition.exception.toString());
    return;
  }

  Serial.printf(
      "จำแนกได้ว่าเป็น: %s | ความมั่นใจ: %.2f | เวลาที่ใช้: %dms\n",
      recognition.match.name.c_str(),
      recognition.match.similarity,
      recognition.benchmark.millis());

  // recognize().isOk() คืน true เสมอแม้จำไม่ได้จริง (name จะเป็น "unknown")
  // ต้องเช็คเองว่าชื่อไม่ใช่ unknown และความมั่นใจถึงเกณฑ์ที่ตั้งไว้จริง
  // (ตรงกับที่แก้ไว้แล้วใน phase4_face_scan_door.cpp)
  bool isKnownPerson = recognition.match.name != "unknown" &&
                        recognition.match.similarity >= 0.87;

  if (isKnownPerson) {
    unlockDoor();
  } else {
    Serial.println(">>> ไม่รู้จักคนนี้ ไม่ปลดล็อก <<<");
  }
}

void unlockDoor() {
  Serial.println(">>> จำได้ ปลดล็อกประตู <<<");
  doorServo.write(UNLOCK_ANGLE);
  delay(DOOR_OPEN_MS);
  doorServo.write(LOCK_ANGLE);
  Serial.println(">>> ล็อกประตูกลับแล้ว <<<");
}
