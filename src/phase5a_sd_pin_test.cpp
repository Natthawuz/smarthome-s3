/*
  phase5a_sd_pin_test.cpp
  -----------------------------------------------------------
  วัตถุประสงค์: หา GPIO ที่ถูกต้องสำหรับช่อง SD card ในตัวของ GPIO Extension Board
  (V2775) — บอร์ด clone นี้ไม่มีเอกสารเผยแพร่ ต้องทดสอบเชิงประจักษ์เหมือนตอนหาพิน
  กล้องใน Phase 0

  Candidate 1: SD_MMC โหมด 1-bit (CLK/CMD/D0) — พินตรงกับที่ Freenove ใช้ในบอร์ด
  ESP32-S3-WROOM CAM อย่างเป็นทางการ (CLK=39, CMD=38, D0=40) น่าจะตรงกับบอร์ดนี้
  เพราะพินกล้องที่ยืนยันแล้วใน Phase 0 ก็ตรงกับ preset ของ Freenove พอดีเป๊ะ
  Candidate 2: SD โหมด SPI ธรรมดา (CS/MOSI/MISO/SCK) ใช้พินว่างที่เหลือ เผื่อ
  Candidate 1 ไม่ตรง

  วิธีใช้:
  1. เลือก environment phase5a-sd-test (Candidate 1) หรือ phase5a-sd-test-candidate2
     จากแถบ PlatformIO ใน VS Code
  2. เสียบ SD card 8GB ให้เรียบร้อย, Upload, เปิด Serial Monitor 115200
  3. ดูผล:
     - "SD MOUNT OK" + เขียน/อ่านไฟล์ทดสอบผ่าน = เจอพินที่ถูกต้องแล้ว จดค่า CANDIDATE ไว้
     - "SD MOUNT FAILED" = ลอง environment ของ Candidate ถัดไป

  หมายเหตุ: กด RESET บอร์ดใหม่ทุกครั้งก่อนลอง Candidate ถัดไป กัน SPI/SDMMC bus
  ค้างสถานะจากรอบก่อน
-----------------------------------------------------------
*/

#include <Arduino.h>

#ifndef CANDIDATE
#define CANDIDATE 1
#endif

#if CANDIDATE == 1
#include "FS.h"
#include "SD_MMC.h"
// Candidate 1: SD_MMC 1-bit mode — พินตรงกับ Freenove ESP32-S3-WROOM CAM (fixed pins)
#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0  40

#elif CANDIDATE == 2
#include "FS.h"
#include "SPI.h"
#include "SD.h"
// Candidate 2: SD SPI mode — ใช้พินว่างที่เหลือหลังจองกล้อง/servo/MPU-6050 แล้ว
#define SD_CS   1
#define SD_MOSI 2
#define SD_MISO 41
#define SD_SCK  42
#endif

void printCardInfo() {
#if CANDIDATE == 1
  uint8_t cardType = SD_MMC.cardType();
  const char *typeStr = "UNKNOWN";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  Serial.printf("ชนิดการ์ด: %s, ขนาด: %lluMB\n", typeStr, SD_MMC.cardSize() / (1024 * 1024));
#elif CANDIDATE == 2
  uint8_t cardType = SD.cardType();
  const char *typeStr = "UNKNOWN";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  Serial.printf("ชนิดการ์ด: %s, ขนาด: %lluMB\n", typeStr, SD.cardSize() / (1024 * 1024));
#endif
}

bool writeReadTest() {
  const char *path = "/esp32_sd_test.txt";
  const char *content = "esp32-elderly-project SD test";

#if CANDIDATE == 1
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();

  f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  String readBack = f.readString();
  f.close();
  SD_MMC.remove(path);
#elif CANDIDATE == 2
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();

  f = SD.open(path, FILE_READ);
  if (!f) return false;
  String readBack = f.readString();
  f.close();
  SD.remove(path);
#endif

  return readBack == content;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.printf("===== ทดสอบพิน SD card: Candidate %d =====\n", CANDIDATE);

  bool mounted = false;

#if CANDIDATE == 1
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  mounted = SD_MMC.begin("/sdcard", true);  // true = 1-bit mode
  Serial.printf("ลองพิน CLK=%d CMD=%d D0=%d (SD_MMC 1-bit)\n", SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
#elif CANDIDATE == 2
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  mounted = SD.begin(SD_CS, SPI);
  Serial.printf("ลองพิน CS=%d MOSI=%d MISO=%d SCK=%d (SD SPI)\n", SD_CS, SD_MOSI, SD_MISO, SD_SCK);
#endif

  if (!mounted) {
    Serial.println("!!! SD MOUNT FAILED — ลองเลือก environment ของ Candidate ถัดไปแล้ว Upload ใหม่ !!!");
    return;
  }

  Serial.println(">>> SD MOUNT OK <<<");
  printCardInfo();

  if (writeReadTest()) {
    Serial.println(">>> เขียน/อ่านไฟล์ทดสอบสำเร็จ <<<");
    Serial.printf(">>> พินชุดนี้ (Candidate %d) ใช้ได้จริง จดไว้ใช้ในโค้ดหลักต่อไป <<<\n", CANDIDATE);
  } else {
    Serial.println("Mount ผ่าน แต่เขียน/อ่านไฟล์ไม่สำเร็จ — อาจต้องลอง Candidate อื่นเช่นกัน");
  }
}

void loop() {
  delay(5000);
}
