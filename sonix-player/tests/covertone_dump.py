#!/usr/bin/env python3
"""Shows what the cover pipeline wrote: every album of ALBUM_COVER with its
240 px and 56 px thumbnails read back from the slot files (header and checksum
checked as covertone.c checks them), the colour it stored, and the UI 2.0
plates painted in its hue -- the row placeholder (SX_T_COVER_TONE), a dark page
and its accent -- as the palette engine will paint them.

    python3 tests/covertone_dump.py <card>/.local/library.db <card>/.local/covers out.png
"""
import math
import sqlite3
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

PACK_HEADER = 64
SLOT_HEADER = 16


def oklch_to_rgb(L, C, h):
    """sx_oklch_to_565 of the handoff, to 8-bit sRGB."""
    hr = math.radians(h)
    a, b = C * math.cos(hr), C * math.sin(hr)
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s

    def gam(x):
        x = 12.92 * x if x <= 0.0031308 else 1.055 * (x ** (1 / 2.4)) - 0.055
        return max(0, min(255, int(round(x * 255))))

    return gam(r), gam(g), gam(bl)


def read_slot(path, side, slot, album_id):
    with open(path, "rb") as f:
        header = f.read(PACK_HEADER)
        if header[:8] != b"SXCOVER1":
            return None, "bad file header"
        fmt, fside, slot_bytes = struct.unpack("<III", header[8:20])
        if fside != side or slot_bytes != SLOT_HEADER + side * side * 2:
            return None, "file of another size"
        f.seek(PACK_HEADER + slot * slot_bytes)
        sid, ssum, sside, magic = struct.unpack("<qIHH", f.read(SLOT_HEADER))
        if sid != album_id or sside != side or magic != 0x5854:
            return None, "slot holds another album"
        raw = f.read(side * side * 2)
    px = struct.unpack("<%dH" % (side * side), raw)
    h = 0x811C9DC5
    for v in px:
        h = (((h << 5) | (h >> 27)) & 0xFFFFFFFF) ^ v
    if h != ssum:
        return None, "checksum"
    img = Image.new("RGB", (side, side))
    img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
    return img, "ok"


def main():
    db, covers, out = sys.argv[1], sys.argv[2], sys.argv[3]
    rows = sqlite3.connect(db).execute(
        "SELECT id, album, cover_h, cover_c, cover_l, kind, thumbs, slot FROM ALBUM_COVER ORDER BY album").fetchall()
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15)
    except OSError:
        font = ImageFont.load_default()
    kinds = {0: "none", 1: "picture", 2: "grey", 3: "failed", 4: "deferred"}
    row_h = 252
    sheet = Image.new("RGB", (960, max(1, len(rows)) * row_h), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, (aid, album, h, c, l, kind, thumbs, slot) in enumerate(rows):
        y = i * row_h + 6
        status = []
        if slot is not None and thumbs & 2:
            img, why = read_slot(f"{covers}/thumbs240.sxt", 240, slot, aid)
            status.append("240 " + why)
            if img:
                sheet.paste(img, (6, y))
        if slot is not None and thumbs & 1:
            img, why = read_slot(f"{covers}/thumbs56.sxt", 56, slot, aid)
            status.append("56 " + why)
            if img:
                sheet.paste(img, (256, y))
        plates = [
            ("stored colour", oklch_to_rgb(l / 255, c / 510, h)),
            ("COVER_TONE", oklch_to_rgb(0.50, 0.090, h)),
            ("D_BG", oklch_to_rgb(0.20, 0.032, h)),
            ("D_ACCENT", oklch_to_rgb(0.84, 0.100, h)),
            ("NP_GRAD_TOP", oklch_to_rgb(0.40, 0.090, h)),
        ]
        for k, (name, rgb) in enumerate(plates):
            x = 330 + k * 124
            d.rectangle([x, y + 70, x + 116, y + 150], fill=rgb)
            d.text((x, y + 154), name, fill=(200, 200, 200), font=font)
        d.text((330, y), f"{album}", fill=(255, 255, 255), font=font)
        d.text((330, y + 22), f"h={h} C={c / 510:.3f} L={l / 255:.3f} {kinds.get(kind, kind)} slot={slot}",
               fill=(200, 200, 200), font=font)
        d.text((330, y + 44), ", ".join(status) or "no thumbnails", fill=(160, 200, 160), font=font)
        print(album, h, round(c / 510, 3), round(l / 255, 3), kinds.get(kind, kind), slot, status)
    sheet.save(out)


if __name__ == "__main__":
    main()
