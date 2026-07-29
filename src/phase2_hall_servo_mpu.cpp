/*
  phase2_hall_servo_mpu.cpp
  -----------------------------------------------------------
  ทดสอบอุปกรณ์ห้องโถง (นอกเหนือจากกล้องที่ยืนยันแล้วใน Phase 0):
  - Servo SG92R: กวาดมุม 0 -> 180 -> 0 องศา วนซ้ำ ยืนยันว่าสั่งมุมได้จริง
  - MPU-6050: อ่านค่า accelerometer + gyroscope พิมพ์ทาง Serial ทุก 500ms

  วิธีดู: เปิด Serial Monitor 115200 baud แล้วดูว่า:
  - Servo ขยับจริงตามมุมที่พิมพ์ (ดูด้วยตา)
  - ค่า MPU อ่านได้สมเหตุสมผล (accel Z ~9.8 ถ้าวางราบ, ค่าขยับเมื่อเขย่า)
-----------------------------------------------------------
*/

#include <Arduino.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "hall_pins.h"

Servo doorServo;
Adafruit_MPU6050 mpu;

int servoAngle = 0;
int servoStep = 5;
unsigned long lastServoMove = 0;
unsigned long lastMpuRead = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== ทดสอบห้องโถง: Servo SG92R + MPU-6050 =====");

  doorServo.attach(SERVO_PIN);
  doorServo.write(servoAngle);
  Serial.printf("Servo attach ที่ GPIO%d แล้ว\n", SERVO_PIN);

  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
  if (!mpu.begin()) {
    Serial.println("!!! MPU-6050 INIT FAILED — เช็คสาย SDA/SCL และไฟเลี้ยง !!!");
  } else {
    Serial.println(">>> MPU-6050 INIT OK <<<");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastServoMove >= 30) {
    lastServoMove = now;
    servoAngle += servoStep;
    if (servoAngle >= 180 || servoAngle <= 0) {
      servoStep = -servoStep;
    }
    doorServo.write(servoAngle);
  }

  if (now - lastMpuRead >= 500) {
    lastMpuRead = now;
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    Serial.printf("Servo=%3d deg | Accel(m/s^2) X=%.2f Y=%.2f Z=%.2f | Gyro(rad/s) X=%.2f Y=%.2f Z=%.2f\n",
                  servoAngle, a.acceleration.x, a.acceleration.y, a.acceleration.z,
                  g.gyro.x, g.gyro.y, g.gyro.z);
  }
}
