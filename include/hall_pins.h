/*
  hall_pins.h — พินอุปกรณ์ห้องโถง (นอกเหนือจากกล้อง ดู camera_pins.h)
  -----------------------------------------------------------
  เลือกพินให้ไม่ชนกับพินกล้อง (4,5,6,7,8,9,10,11,12,13,15,16,17,18)
  และไม่ชนพินต้องห้าม/ต้องระวังตามเอกสารโครงการ:
  IO0, IO3, IO45, IO46 (strapping), IO19/IO20 (native USB), RX(43)/TX(44) (UART0),
  IO35-37 (เสี่ยงชน Octal PSRAM), IO48 (มี LED บนบอร์ด)
-----------------------------------------------------------
*/
#pragma once

#define SERVO_PIN    14  // Servo SG92R (PWM)
#define MPU_SDA_PIN  21  // MPU-6050 I2C SDA
#define MPU_SCL_PIN  47  // MPU-6050 I2C SCL
#define EARTHQUAKE_BUZZER_PIN 2  // ใช้ขาเดียวกับ phase5d/phase7 (เฟสแยกกัน ไม่รันพร้อมกัน)
