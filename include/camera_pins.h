/*
  camera_pins.h
  -----------------------------------------------------------
  พินกล้อง OV2640/OV5640 ที่ยืนยันแล้วบนฮาร์ดแวร์จริง
  บอร์ด: ESP32-S3-CAM (N16R8) + GPIO Extension Board (V2775)

  ยืนยันจากการทดสอบจริงด้วย camera_pin_test (Phase 0):
  Candidate 1 (อิง CAMERA_MODEL_ESP32S3_EYE) — CAMERA INIT OK,
  ถ่ายภาพได้จริง JPEG 320x240

  ห้ามใช้พินกลุ่มนี้ซ้ำกับอุปกรณ์อื่นในเฟสถัดไป: 15, 4, 5, 11, 9, 8, 10, 12, 18, 17, 16, 6, 7, 13
-----------------------------------------------------------
*/
#pragma once

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y2_GPIO_NUM    11
#define Y3_GPIO_NUM    9
#define Y4_GPIO_NUM    8
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    12
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    16
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13
