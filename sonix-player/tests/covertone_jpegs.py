#!/usr/bin/env python3
"""Makes the JPEGs tests/covertone_test.c times: photograph-like covers (a sky,
a field, a figure, texture) at the sizes album art comes in, a flat graphic
one, a progressive one, and the 400x400 calibration picture.

    python3 tests/covertone_jpegs.py /tmp/covertone
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter


def photo(size, seed=1):
    rnd = random.Random(seed)
    img = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        t = y / size
        if t < 0.5:
            c = (int(150 + 40 * t), int(180 + 30 * t), int(215 - 20 * t))
        else:
            c = (int(70 - 40 * (t - 0.5)), int(130 - 60 * (t - 0.5)), 40)
        d.line([(0, y), (size, y)], fill=c)
    d.rectangle([size * 0.43, size * 0.32, size * 0.56, size * 0.80], fill=(210, 35, 35))
    d.ellipse([size * 0.70, size * 0.08, size * 0.85, size * 0.23], fill=(250, 230, 160))
    # Texture: speckles over the whole picture, then a slight blur, so the file
    # is as busy as a photograph and not a flat graphic.
    px = img.load()
    for _ in range(size * size // 3):
        x, y = rnd.randrange(size), rnd.randrange(size)
        r, g, b = px[x, y]
        n = rnd.randint(-40, 40)
        px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)))
    return img.filter(ImageFilter.GaussianBlur(0.6))


def flat(size):
    img = Image.new("RGB", (size, size), (240, 236, 228))
    d = ImageDraw.Draw(img)
    d.ellipse([size * 0.2, size * 0.2, size * 0.8, size * 0.8], fill=(20, 90, 170))
    d.rectangle([0, size * 0.85, size, size], fill=(30, 30, 30))
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/covertone"
    os.makedirs(out, exist_ok=True)
    photo(400, 7).save(os.path.join(out, "cal400.jpg"), quality=90)
    photo(600).save(os.path.join(out, "photo600.jpg"), quality=90)
    photo(1500).save(os.path.join(out, "photo1500.jpg"), quality=90)
    photo(3000).save(os.path.join(out, "photo3000.jpg"), quality=88)
    flat(600).save(os.path.join(out, "flat600.jpg"), quality=92)
    photo(1200, 3).save(os.path.join(out, "prog1200.jpg"), quality=90, progressive=True)
    for name in sorted(os.listdir(out)):
        print(name, os.path.getsize(os.path.join(out, name)) // 1024, "KB")


if __name__ == "__main__":
    main()
