#!/usr/bin/env python3
"""Builds a test card for the cover pipeline: albums of short tagged FLACs with
every kind of picture the pipeline meets -- a cover.jpg of 600 px and of
1500 px, a 1000 px JPEG embedded in the tracks, a progressive JPEG, a PNG, a
grey photograph, and none at all -- for timing a full scan with and without
the covers' pass, in the emulator or on the device.

    python3 tests/covertone_card.py <dir> [albums]    # needs sox, metaflac, PIL

<dir>/Music/Bench NN/... is written; copy it to a card's Music folder.
"""
import os
import random
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter


def photo(size, seed, grey=False):
    rnd = random.Random(seed)
    base = [rnd.randint(30, 230) for _ in range(3)]
    img = Image.new("RGB", (size, size), tuple(base))
    d = ImageDraw.Draw(img)
    for _ in range(14):
        x0, y0 = rnd.randint(0, size), rnd.randint(0, size)
        r = rnd.randint(size // 12, size // 3)
        c = tuple(rnd.randint(0, 255) for _ in range(3))
        (d.ellipse if rnd.random() < 0.5 else d.rectangle)([x0 - r, y0 - r, x0 + r, y0 + r], fill=c)
    px = img.load()
    for _ in range(size * size // 4):
        x, y = rnd.randrange(size), rnd.randrange(size)
        p = px[x, y]
        n = rnd.randint(-35, 35)
        px[x, y] = tuple(max(0, min(255, v + n)) for v in p)
    img = img.filter(ImageFilter.GaussianBlur(0.7))
    return img.convert("L").convert("RGB") if grey else img


def main():
    out = sys.argv[1]
    albums = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    tracks = 8
    os.makedirs(out, exist_ok=True)
    tone = os.path.join(out, "tone.flac")
    if not os.path.exists(tone):
        subprocess.run(["sox", "-n", "-r", "44100", "-b", "16", "-c", "2", tone, "synth", "1", "sine", "440"],
                       check=True)
    kinds = ["file600", "file1500", "embedded1000", "progressive", "png", "grey", "none"]
    weights = [6, 6, 3, 1, 1, 1, 2]
    rnd = random.Random(7)
    for a in range(albums):
        kind = rnd.choices(kinds, weights)[0]
        folder = os.path.join(out, "Music", f"Bench {a:02d}")
        os.makedirs(folder, exist_ok=True)
        pic = None
        if kind == "file600":
            photo(600, a).save(os.path.join(folder, "cover.jpg"), quality=90)
        elif kind == "file1500":
            photo(1500, a).save(os.path.join(folder, "cover.jpg"), quality=90)
        elif kind == "progressive":
            photo(1200, a).save(os.path.join(folder, "folder.jpg"), quality=90, progressive=True)
        elif kind == "png":
            photo(800, a).save(os.path.join(folder, "cover.png"))
        elif kind == "grey":
            photo(600, a, grey=True).save(os.path.join(folder, "cover.jpg"), quality=90)
        elif kind == "embedded1000":
            pic = os.path.join(out, f"embed{a}.jpg")
            photo(1000, a).save(pic, quality=90)
        for t in range(1, tracks + 1):
            path = os.path.join(folder, f"{t:02d}.flac")
            with open(tone, "rb") as src, open(path, "wb") as dst:
                dst.write(src.read())
            args = ["metaflac", "--remove-all-tags", f"--set-tag=TITLE=Track {t}", f"--set-tag=ARTIST=Bench Artist {a % 9}",
                    f"--set-tag=ALBUM=Bench Album {a:02d} ({kind})", f"--set-tag=TRACKNUMBER={t}",
                    f"--set-tag=DATE={1970 + a}"]
            if pic:
                args.append(f"--import-picture-from={pic}")
            subprocess.run(args + [path], check=True)
        if pic:
            os.remove(pic)
        print(folder, kind)


if __name__ == "__main__":
    main()
