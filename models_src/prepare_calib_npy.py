import os
import glob
import argparse
import cv2
import numpy as np


def resize_with_reflect_pad(img, target_size=128):
    h, w = img.shape[:2]
    if h == 0 or w == 0:
        raise ValueError("Invalid image with zero size")

    scale = float(target_size) / max(h, w)
    new_w = max(1, int(round(w * scale)))
    new_h = max(1, int(round(h * scale)))

    resized = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_LINEAR)

    pad_w = target_size - new_w
    pad_h = target_size - new_h

    left = pad_w // 2
    right = pad_w - left
    top = pad_h // 2
    bottom = pad_h - top

    padded = cv2.copyMakeBorder(
        resized,
        top,
        bottom,
        left,
        right,
        borderType=cv2.BORDER_REFLECT_101,
    )

    return padded


def preprocess_image(img_bgr, target_size=128):
    # BGR -> RGB
    img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)

    # 等比缩放 + 反射填充到 128x128
    img_rgb = resize_with_reflect_pad(img_rgb, target_size)

    # HWC -> CHW
    tensor = img_rgb.astype(np.float32) / 255.0
    tensor = np.transpose(tensor, (2, 0, 1))  # (3, 128, 128)
    tensor = np.expand_dims(tensor, axis=0)   # (1, 3, 128, 128)

    return tensor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_dir", required=True, help="Directory of face images")
    parser.add_argument("--output_dir", required=True, help="Directory to save .npy files")
    parser.add_argument("--limit", type=int, default=32, help="Max number of images to process")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    exts = ["*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp"]
    image_files = []
    for ext in exts:
        image_files.extend(glob.glob(os.path.join(args.input_dir, ext)))
        image_files.extend(glob.glob(os.path.join(args.input_dir, ext.upper())))

    image_files = sorted(image_files)

    if not image_files:
        raise RuntimeError(f"No image files found in {args.input_dir}")

    image_files = image_files[:args.limit]

    saved = 0
    for idx, img_path in enumerate(image_files):
        img = cv2.imread(img_path)
        if img is None:
            print(f"[WARN] skip unreadable image: {img_path}")
            continue

        try:
            tensor = preprocess_image(img, target_size=128)
        except Exception as e:
            print(f"[WARN] skip {img_path}: {e}")
            continue

        base = os.path.splitext(os.path.basename(img_path))[0]
        out_path = os.path.join(args.output_dir, f"{idx:03d}_{base}.npy")
        np.save(out_path, tensor)
        saved += 1
        print(f"[OK] {img_path} -> {out_path} {tensor.shape} {tensor.dtype}")

    print(f"\nDone. Saved {saved} calibration samples to: {args.output_dir}")


if __name__ == "__main__":
    main()