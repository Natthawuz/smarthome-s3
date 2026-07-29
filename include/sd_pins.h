/*
  sd_pins.h — พิน SD card (ช่องในตัวของ GPIO Extension Board V2775) ที่ยืนยันแล้ว
  -----------------------------------------------------------
  ยืนยันจากการทดสอบจริงด้วย phase5a_sd_pin_test (Phase 5A):
  Candidate 1 (SD_MMC 1-bit, พินตรงกับ Freenove ESP32-S3-WROOM CAM) — SD MOUNT OK,
  เขียน/อ่านไฟล์ทดสอบสำเร็จ, การ์ด SDHC 7583MB (การ์ด 8GB)

  ห้ามใช้พินกลุ่มนี้ซ้ำกับอุปกรณ์อื่น: 38, 39, 40
-----------------------------------------------------------
*/
#pragma once

#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0  40
