#!/usr/bin/env python3
"""
Assembles the Wi-Fi transfer page and turns it into a C string the player can
write out at runtime.

    web/index.html      the template, with three placeholders
    web/icons/*.svg     one Lucide glyph per file
    web/img/*.png       the logo and the favicon
        |
        v
    web/build/index.html    the finished page -- open this in a browser
    src/system/net/webpage.h    the same bytes, as a C string literal

Assembled rather than linked because the served page has to be ONE file: the
player writes a single file into thttpd's document root and nothing else has to
be installed on the device, and a page with no subresources cannot half-load on
a flaky link.

Both outputs are checked in, so building the player does NOT need Python.
Re-run this whenever the page, an icon or an image changes:

    python3 tools/web_to_c.py
"""

import base64
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(REPO_ROOT, "web")
TEMPLATE = os.path.join(WEB, "index.html")
ICON_DIR = os.path.join(WEB, "icons")
IMG_DIR = os.path.join(WEB, "img")
BUILD = os.path.join(WEB, "build", "index.html")
OUT_H = os.path.join(REPO_ROOT, "src", "system", "webpage.h")

# Long string literals are legal C, but a single 70 KB one is a pain to read in
# a diff. Chopped into lines, which the compiler concatenates back at no cost.
CHUNK = 100


def symbol_id(filename: str) -> str:
    """upload-web.svg -> upload, chevron-left.svg -> chevron-left.

    The "-web" suffix is how the icons arrived; it says where they are used, not
    what they are, so it does not belong in the id the page refers to.
    """
    name = os.path.splitext(filename)[0]
    return name[:-4] if name.endswith("-web") else name


def inline_icons() -> str:
    """Every icon as a <symbol>, presentation attributes stripped.

    The stroke/fill properties live in one CSS rule on .svgicon instead: they
    are inheritable, so they reach into the <use> content, and the glyphs then
    take their colour from whatever they sit in -- the accent on a folder, dim
    grey on a file, red on "Elimina".
    """
    out = []
    for filename in sorted(os.listdir(ICON_DIR)):
        if not filename.endswith(".svg"):
            continue

        with open(os.path.join(ICON_DIR, filename), encoding="utf-8") as f:
            svg = f.read()

        open_tag = re.search(r"<svg\b[^>]*>", svg)
        if not open_tag:
            sys.exit("%s: no <svg> element" % filename)

        viewbox = re.search(r'viewBox="([^"]+)"', open_tag.group(0))
        viewbox = viewbox.group(1) if viewbox else "0 0 24 24"

        inner = svg[open_tag.end():svg.rindex("</svg>")].strip()
        inner = re.sub(r"\s+", " ", inner)

        out.append('<symbol id="%s" viewBox="%s">%s</symbol>' % (symbol_id(filename), viewbox, inner))

    if not out:
        sys.exit("no icons in %s" % ICON_DIR)
    return "\n".join(out)


def data_url(name: str) -> str:
    path = os.path.join(IMG_DIR, name)
    with open(path, "rb") as f:
        raw = f.read()
    ext = os.path.splitext(name)[1].lower()
    mime = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
            ".gif": "image/gif", ".svg": "image/svg+xml", ".webp": "image/webp"}.get(ext)
    if not mime:
        sys.exit("%s: do not know the type" % name)
    return "data:%s;base64,%s" % (mime, base64.b64encode(raw).decode("ascii"))


def escape(chunk: bytes) -> str:
    out = []
    for b in chunk:
        c = chr(b)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            pass  # normalise CRLF away
        elif c == "\t":
            out.append("\\t")
        elif c == "?":
            # "??" starts a trigraph in strict C. Nothing in the page needs one.
            out.append("\\?")
        elif 32 <= b < 127:
            out.append(c)
        else:
            # A UTF-8 byte. Octal, not hex: "\xNN" swallows the hex digits that
            # follow it, and this page is full of accented letters next to
            # ordinary ones.
            out.append("\\%03o" % b)
    return "".join(out)


def main() -> None:
    with open(TEMPLATE, encoding="utf-8") as f:
        page = f.read()

    for placeholder, value in (("<!--ICONS-->", inline_icons()),
                               ("<!--LOGO-->", data_url("logo-web.png")),
                               ("<!--FAVICON-->", data_url("favicon-web.png"))):
        # Exactly one of each. The template's own header explains them without
        # writing them out, because a comment containing "-->" ends there and
        # the rest of it lands on the page as text.
        if page.count(placeholder) != 1:
            sys.exit("template has %d x %s, want exactly 1" % (page.count(placeholder), placeholder))
        page = page.replace(placeholder, value)

    data = page.encode("utf-8")

    os.makedirs(os.path.dirname(BUILD), exist_ok=True)
    with open(BUILD, "wb") as f:
        f.write(data)

    lines = []
    start = 0
    while start < len(data):
        # Never split a UTF-8 sequence: back off to a lead byte boundary.
        end = min(start + CHUNK, len(data))
        while end < len(data) and (data[end] & 0xC0) == 0x80:
            end += 1
        lines.append('\t"%s"' % escape(data[start:end]))
        start = end

    with open(OUT_H, "w") as f:
        f.write("// Generated by tools/web_to_c.py -- do not edit.\n")
        f.write("//\n")
        f.write("// The Wi-Fi transfer page, assembled from web/index.html, web/icons/*.svg and\n")
        f.write("// web/img/*.png, and carried in the binary so the player can write it into\n")
        f.write("// thttpd's document root itself. Edit those and re-run the script.\n\n")
        f.write("#ifndef WEBPAGE_H\n#define WEBPAGE_H\n\n")
        f.write("static const char WIFITRANSFER_PAGE[] =\n")
        f.write("\n".join(lines))
        f.write(";\n\n#endif /* WEBPAGE_H */\n")

    print("wrote %s and %s (%d KB of page, %d lines)" % (BUILD, OUT_H, len(data) // 1024, len(lines)))


if __name__ == "__main__":
    main()
