/*
  main.cpp — Phase 0: ยืนยันกล้องด้วยพินที่ยืนยันแล้ว (camera_pins.h)
  -----------------------------------------------------------
  พินกล้องผ่านการทดสอบจริงแล้ว (ดู camera_pins.h) ไฟล์นี้เหลือไว้เป็น
  smoke test ยืนยันว่ากล้องยังทำงานปกติทุกครั้งที่แก้ไข platformio.ini
  หรือย้ายไปบอร์ดอื่น ก่อนเริ่มเขียนเฟสถัดไปทับไฟล์นี้
-----------------------------------------------------------
*/

#include <Arduino.h>
#include "esp_camera.h"
#include "camera_pins.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ทดสอบกล้อง (พินยืนยันแล้ว) =====");

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
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality  = 12;
  config.fb_count      = 1;
  config.fb_location   = CAMERA_FB_IN_PSRAM;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("!!! CAMERA INIT FAILED (error 0x%x) !!!\n", err);
    return;
  }

  Serial.println(">>> CAMERA INIT OK <<<");

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Init ผ่าน แต่ถ่ายภาพไม่สำเร็จ (fb_get ล้มเหลว)");
    return;
  }

  Serial.printf("ถ่ายภาพสำเร็จ! ขนาดไฟล์ JPEG = %u bytes, ความกว้าง x สูง = %u x %u\n",
                fb->len, fb->width, fb->height);
  esp_camera_fb_return(fb);
}

void loop() {
  delay(5000);
}
