/*
  phase5e_psram_test.cpp
  -----------------------------------------------------------
  ทดสอบ PSRAM ล้วนๆ แยกจากกล้อง/SD/WiFi เพื่อวินิจฉัยว่า PSRAM เสียจริง
  หรือแค่ระบบรวมกันใช้ทรัพยากรเกิน
-----------------------------------------------------------
*/
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ทดสอบ PSRAM ล้วนๆ =====");
  Serial.printf("psramFound(): %s\n", psramFound() ? "true" : "false");
  Serial.printf("ESP.getPsramSize(): %u bytes\n", ESP.getPsramSize());
  Serial.printf("ESP.getFreePsram(): %u bytes\n", ESP.getFreePsram());
  Serial.printf("ESP.getHeapSize() (internal RAM): %u bytes\n", ESP.getHeapSize());
  Serial.printf("ESP.getFreeHeap() (internal RAM): %u bytes\n", ESP.getFreeHeap());

  void *testBuf = heap_caps_malloc(1024 * 1024, MALLOC_CAP_SPIRAM);
  Serial.printf("ลองจอง 1MB จาก PSRAM: %s\n", testBuf != nullptr ? "สำเร็จ" : "ล้มเหลว");
  if (testBuf != nullptr) {
    free(testBuf);
  }
}

void loop() {
  delay(1000);
}
