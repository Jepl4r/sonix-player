#!/usr/bin/env python3
"""Add draw-task identity to the pinned LVGL 9.6 span renderer.

Generate a build-local copy, leaving the dependency checkout untouched. Span
labels otherwise omit base.obj, so DRAW_TASK_ADDED cannot style their selection.
The span and byte offset also preserve identity when bidi copies the text.
"""
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
patches = [
    ('    lv_draw_label_dsc_init(&label_draw_dsc);',
     '    lv_draw_label_dsc_init(&label_draw_dsc);\n    label_draw_dsc.base.obj = obj;'),
    ('            label_draw_dsc.text = bidi_txt;',
     '            label_draw_dsc.base.user_data = pinfo->span;\n'
     '            label_draw_dsc.base.id1 = (uint32_t)(pinfo->txt - pinfo->span->txt);\n'
     '            label_draw_dsc.text = bidi_txt;'),
]
for before, after in patches:
    if source.count(before) != 1:
        raise SystemExit('LVGL span renderer changed; review the draw-task compatibility patch')
    source = source.replace(before, after, 1)
Path(sys.argv[2]).write_text(source)
