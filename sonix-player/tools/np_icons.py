#!/usr/bin/env python3
"""
The Now Playing page's own glyphs (2.0): the small chevrons of the "Up next"
chip and the signal chain, and the glyphs of the player's and the queue's
bottom sheets.

A separate output from tools/svg_to_lvgl.py on purpose: icons.c is one
generated file that every page's icons go through, and the player's few
belong with the player rather than in a 200 000-line file several people
regenerate at once. Same rendering (white ARGB8888 for recolouring), same
SVGs (assets/icons), written to src/gui/nowplaying/npicons.{c,h}:

    python3 tools/np_icons.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svg_to_lvgl as gen  # noqa: E402

OUT_C = os.path.join(gen.REPO_ROOT, "src", "gui", "nowplaying", "npicons.c")
OUT_H = os.path.join(gen.REPO_ROOT, "src", "gui", "nowplaying", "npicons.h")

# (svg file, C identifier, pixel size)
ICONS = [
    # The "Up next" chip's arrow and the chain line's open/close mark, at the
    # size of the 19-20 px text beside them.
    ("chevron-right.svg", "chevron_right_sm", 22),
    ("chevron-up.svg", "chevron_up_sm", 20),
    ("chevron-down.svg", "chevron_down_sm", 20),
    # The sheets' glyph column (sx_sheet.c: a 36 px slot).
    ("square.svg", "stop_after", 28),
    ("moon.svg", "sleep", 30),
    ("list-music.svg", "queue", 30),
    ("list-plus.svg", "add_playlist", 30),
    ("disc.svg", "album", 30),
    ("user.svg", "artist", 30),
    ("info.svg", "details", 30),
    ("settings-2.svg", "settings", 30),
    ("text-quote.svg", "lyrics", 30),
    ("shuffle.svg", "shuffle", 30),
    ("play.svg", "play", 28),
    ("arrow-up.svg", "move_up", 30),
    ("arrow-down.svg", "move_down", 30),
    ("list-start.svg", "play_next", 30),
    ("x.svg", "remove", 30),
    ("list-x.svg", "clear", 30),
]


def main():
    parts_c = []
    parts_h = []
    for filename, name, size in ICONS:
        path = os.path.join(gen.ICON_DIR, filename)
        if not os.path.exists(path):
            sys.exit("missing icon: %s" % path)
        data, w, h = gen.render(path, size)
        parts_c.append(
            "// %s, %dx%d\n"
            "static const uint8_t npicon_%s_data[] = {\n%s\n};\n\n"
            "const lv_image_dsc_t npicon_%s = {\n"
            "\t.header = {\n"
            "\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "\t\t.cf = LV_COLOR_FORMAT_ARGB8888,\n"
            "\t\t.w = %d,\n"
            "\t\t.h = %d,\n"
            "\t\t.stride = %d,\n"
            "\t},\n"
            "\t.data_size = sizeof(npicon_%s_data),\n"
            "\t.data = npicon_%s_data,\n"
            "};\n" % (filename, w, h, name, gen.c_array(data), name, w, h, w * 4, name, name)
        )
        parts_h.append("extern const lv_image_dsc_t npicon_%s;" % name)

    header = (
        "/*\n"
        " * GENERATED FILE -- do not edit by hand.\n"
        " * Produced from the SVGs in assets/icons by tools/np_icons.py.\n"
        " *\n"
        " * White ARGB8888 bitmaps: tint them with lv_obj_set_style_image_recolor().\n"
        " */\n\n"
    )
    with open(OUT_C, "w") as f:
        f.write(header)
        f.write('#include "npicons.h"\n\n#include <stdint.h>\n\n')
        f.write("\n".join(parts_c))
    with open(OUT_H, "w") as f:
        f.write(header)
        f.write("#ifndef NPICONS_H\n#define NPICONS_H\n\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write("\n".join(parts_h))
        f.write("\n\n#endif // NPICONS_H\n")
    print("wrote %s and %s" % (OUT_C, OUT_H))


if __name__ == "__main__":
    main()
