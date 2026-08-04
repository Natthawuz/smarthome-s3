"""
export_float32.py
-----------------------------------------------------------
โหลดโมเดล Keras ที่เทรนไว้แล้ว (fall_model.keras) มาแปลงเป็น TFLite แบบ
float32 (ไม่ quantize) สำหรับใช้กับไลบรารี ArduTFLite บน ESP32-S3
(ArduTFLite รองรับแค่โมเดล float32 เท่านั้น ไม่รองรับ INT8 quantized)
-----------------------------------------------------------
"""

import pathlib
import tensorflow as tf

OUTPUT_DIR = pathlib.Path(__file__).parent / "output"
model = tf.keras.models.load_model(OUTPUT_DIR / "fall_model.keras")

converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

tflite_path = OUTPUT_DIR / "fall_model_float32.tflite"
tflite_path.write_bytes(tflite_model)
print(f"บันทึก TFLite float32 ({len(tflite_model)} bytes) ที่ {tflite_path}")

header_path = OUTPUT_DIR / "fall_model_data.h"
with open(header_path, "w") as f:
    f.write("// สร้างอัตโนมัติจาก export_float32.py — อย่าแก้มือ\n")
    f.write("#pragma once\n\n")
    f.write(f"const unsigned int fall_model_data_len = {len(tflite_model)};\n")
    f.write("alignas(8) const unsigned char fall_model_data[] = {\n")
    for i in range(0, len(tflite_model), 12):
        chunk = tflite_model[i:i + 12]
        f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")
print(f"บันทึก C header ที่ {header_path}")
