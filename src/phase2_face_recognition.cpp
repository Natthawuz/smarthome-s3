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
     - พิมพ์ e แล้ว Enter เพื่อบันทึกใบหน้าใหม่ (ต้องใส่ชื่อ) — แนะนำให้ enroll คนเดิม 2-3 ครั้ง มุมต่างกันเล็กน้อย
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
  recognition.confidence(0.85);

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

void enroll() {
  String name = prompt("ใส่ชื่อคนที่จะบันทึก:");

  if (recognition.enroll(name).isOk())
    Serial.println("บันทึกสำเร็จ!");
  else
    Serial.println(recognition.exception.toString());
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

  unlockDoor();
}

void unlockDoor() {
  Serial.println(">>> จำได้ ปลดล็อกประตู <<<");
  doorServo.write(UNLOCK_ANGLE);
  delay(DOOR_OPEN_MS);
  doorServo.write(LOCK_ANGLE);
  Serial.println(">>> ล็อกประตูกลับแล้ว <<<");
}
