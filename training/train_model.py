"""
train_model.py
-----------------------------------------------------------
เทรนโมเดล CNN เล็กจำแนกท่ายืน/นั่ง/ล้ม จากภาพใน ../dataset/{standing,sitting,fallen}
แล้วแปลงเป็น TensorFlow Lite (quantized INT8) พร้อมส่งออกเป็น C header
สำหรับฝังในเฟิร์มแวร์ ESP32-S3 ต่อไป

รัน:
    ./train_env/bin/python training/train_model.py
-----------------------------------------------------------
"""

import sys
import pathlib
import numpy as np
import tensorflow as tf
from PIL import Image

IMG_SIZE = 96
DATASET_DIR = pathlib.Path(__file__).parent.parent / "dataset"
LABELS = ["standing", "sitting", "fallen"]
OUTPUT_DIR = pathlib.Path(__file__).parent / "output"
OUTPUT_DIR.mkdir(exist_ok=True)

# --real-only : ใช้เฉพาะภาพจากกล้องจริง ตัดภาพชุด urfd_* (URFD dataset) ออก
# เพื่อทดสอบว่าปัญหาโมเดลไม่ generalize เกิดจาก domain mismatch จริงไหม
REAL_ONLY = "--real-only" in sys.argv


def load_dataset():
    images = []
    labels = []
    for label_idx, label in enumerate(LABELS):
        folder = DATASET_DIR / label
        files = [f for f in folder.iterdir() if f.suffix.lower() in (".jpg", ".jpeg", ".png")]
        if REAL_ONLY:
            files = [f for f in files if not f.name.startswith("urfd_")]
        print(f"{label}: {len(files)} ภาพ")
        for f in files:
            try:
                img = Image.open(f).convert("RGB").resize((IMG_SIZE, IMG_SIZE))
                images.append(np.array(img, dtype=np.uint8))
                labels.append(label_idx)
            except Exception as e:
                print(f"  ข้ามไฟล์เสีย {f.name}: {e}")
    return np.array(images), np.array(labels)


def build_model():
    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(IMG_SIZE, IMG_SIZE, 3)),
        tf.keras.layers.Rescaling(1.0 / 255),
        tf.keras.layers.Conv2D(8, 3, activation="relu"),
        tf.keras.layers.MaxPooling2D(),
        tf.keras.layers.Conv2D(16, 3, activation="relu"),
        tf.keras.layers.MaxPooling2D(),
        tf.keras.layers.Conv2D(32, 3, activation="relu"),
        tf.keras.layers.MaxPooling2D(),
        tf.keras.layers.Flatten(),
        tf.keras.layers.Dense(32, activation="relu"),
        tf.keras.layers.Dropout(0.3),
        tf.keras.layers.Dense(len(LABELS), activation="softmax"),
    ])
    model.compile(optimizer="adam", loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    return model


def representative_dataset_gen(train_images):
    def gen():
        for i in range(min(200, len(train_images))):
            img = train_images[i:i + 1].astype(np.float32) / 255.0
            yield [img]
    return gen


def main():
    print("===== โหลดข้อมูล =====")
    images, labels = load_dataset()
    print(f"รวม: {len(images)} ภาพ")

    # แบ่ง train/val แยกทีละหมวด (stratified) กันไม่ให้หมวดใดหมวดหนึ่งหลุดไปกองอยู่
    # ฝั่งเดียวหมด (โดยเฉพาะ dataset เล็กแบบนี้ สำคัญมาก ไม่งั้น validation set
    # อาจไม่มีบางหมวดเลย ทำให้ผลวัดไม่น่าเชื่อถือ)
    rng = np.random.RandomState(42)
    train_x_parts, val_x_parts, train_y_parts, val_y_parts = [], [], [], []
    for label_idx in range(len(LABELS)):
        idx = np.where(labels == label_idx)[0]
        idx = rng.permutation(idx)
        split = max(1, int(len(idx) * 0.8)) if len(idx) > 1 else len(idx)
        train_idx, val_idx = idx[:split], idx[split:]
        train_x_parts.append(images[train_idx])
        val_x_parts.append(images[val_idx])
        train_y_parts.append(labels[train_idx])
        val_y_parts.append(labels[val_idx])

    train_x = np.concatenate(train_x_parts)
    val_x = np.concatenate(val_x_parts)
    train_y = np.concatenate(train_y_parts)
    val_y = np.concatenate(val_y_parts)

    shuffle_train = rng.permutation(len(train_x))
    train_x, train_y = train_x[shuffle_train], train_y[shuffle_train]

    print(f"train: {len(train_x)}, val: {len(val_x)}")
    for label_idx, label in enumerate(LABELS):
        print(f"  val {label}: {(val_y == label_idx).sum()} ภาพ")

    print("\n===== เทรนโมเดล =====")
    model = build_model()
    model.summary()

    early_stop = tf.keras.callbacks.EarlyStopping(
        monitor="val_accuracy", patience=8, restore_best_weights=True)

    model.fit(
        train_x, train_y,
        validation_data=(val_x, val_y),
        epochs=50,
        batch_size=16,
        callbacks=[early_stop],
    )

    print("\n===== ผลลัพธ์บน validation set =====")
    val_loss, val_acc = model.evaluate(val_x, val_y)
    print(f"Validation accuracy: {val_acc:.2%}")

    preds = np.argmax(model.predict(val_x), axis=1)
    print("\nConfusion matrix (แถว=จริง, คอลัมน์=ทำนาย) [standing, sitting, fallen]:")
    cm = np.zeros((3, 3), dtype=int)
    for t, p in zip(val_y, preds):
        cm[t][p] += 1
    print(cm)

    keras_path = OUTPUT_DIR / "fall_model.keras"
    model.save(keras_path)
    print(f"\nบันทึกโมเดล Keras ที่ {keras_path}")

    print("\n===== แปลงเป็น TFLite (INT8 quantized) =====")
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset_gen(train_x)
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.uint8
    converter.inference_output_type = tf.uint8
    tflite_model = converter.convert()

    tflite_path = OUTPUT_DIR / "fall_model.tflite"
    tflite_path.write_bytes(tflite_model)
    print(f"บันทึก TFLite ({len(tflite_model)} bytes) ที่ {tflite_path}")

    # แปลงเป็น C header สำหรับฝังในเฟิร์มแวร์
    header_path = OUTPUT_DIR / "fall_model_data.h"
    with open(header_path, "w") as f:
        f.write("// สร้างอัตโนมัติจาก train_model.py — อย่าแก้มือ\n")
        f.write("#pragma once\n\n")
        f.write(f"const unsigned int fall_model_data_len = {len(tflite_model)};\n")
        f.write("alignas(8) const unsigned char fall_model_data[] = {\n")
        for i in range(0, len(tflite_model), 12):
            chunk = tflite_model[i:i + 12]
            f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n")
    print(f"บันทึก C header ที่ {header_path}")

    print("\n===== เสร็จสิ้น =====")
    print(f"Validation accuracy: {val_acc:.2%}")
    print(f"ขนาดโมเดล TFLite: {len(tflite_model)} bytes")


if __name__ == "__main__":
    main()
