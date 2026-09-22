#!/usr/bin/env python3
"""
Removes the one-contact cap from the R1's touchscreen driver.

The Hynitron CST8xx panel tracks two fingers, and cst8xx_touch.ko reports one.
Unlike the R3 Pro II's gt9xx, the cap here is not a rate limit and not the
insmod line: hyn_ts_init() writes the number 1 into the driver's own
max_touch_number field with an immediate, a single instruction, and the module
parameter of the same name is never read by any code in the module.

That one number is the whole of the limit. It is read back in two places and
decides five things:

    hyn_ts_init        buffer  = kmalloc(6 * n + 4)     the I2C frame
                       read_len = 6 * n + 3
                       contacts = kmalloc(28 * n)       the parsed contacts
    hyn_irq_handler    if (n < (buf[2] & 0xf)) return;  drops a fuller frame
                       for (i = 0; i < n; i++)          the parse loop

So raising the immediate from 1 to 2 widens the I2C read, the two allocations,
the sanity check and the loop together -- 6*2+3 = 15 bytes, which is exactly
the two-contact frame (buf[3..14]). There is nothing else to change, and
nothing left inconsistent.

Two is the panel, not a guess: with the immediate set to 5 a three-finger test
only ever produced tracking ids 0 and 1.

    python3 tools/cst8xx_multitouch_patch.py [directory]
    python3 tools/cst8xx_multitouch_patch.py --check  [directory]
    python3 tools/cst8xx_multitouch_patch.py --revert [directory]

The directory is the one holding cst8xx_touch.ko and cst8xx_touch.sh -- the
module_driver of an unpacked rootfs, or of the packer's assets tree. With no
directory the script works on whatever sits beside itself, so the two files can
simply be copied next to it.

Both files are backed up next to themselves with a .orig suffix before anything
is written, and --revert puts them back.

PATCHES.md has the full account, this driver and the R3 Pro II's.
"""

import argparse
import os
import re
import shutil
import struct
import sys

MODULE = "cst8xx_touch.ko"
SCRIPT = "cst8xx_touch.sh"
BACKUP_SUFFIX = ".orig"

# Contacts to ask for. See the note above on why it is two and not five.
MAX_TOUCH = 2

# The instruction that sets the cap, with the store that follows it:
#
#     addiu v0, zero, 1      the number
#     sw    v0, 0x58(s1)     pdata->max_touch_number = v0
#
# The store is matched only to place the immediate. `li v0, 1` alone appears
# eight times in this module, so it is not an anchor; the pair appears once.
# Little-endian words, as they sit in the file.
ANCHOR = struct.pack("<II", 0x24020001, 0xAE220058)
IMMEDIATE_OFFSET = 0  # where the addiu sits inside ANCHOR

# The same pair with the immediate raised. Built rather than written out, so
# MAX_TOUCH stays the one place the number is said.
ANCHOR_PATCHED = struct.pack("<II", 0x24020000 | MAX_TOUCH, 0xAE220058)


def fail(message):
    sys.exit("cst8xx_multitouch_patch: " + message)


def find_once(data, pattern, what):
    """The single offset of `pattern`, or None. More than one is an error."""
    hits = []
    start = 0
    while True:
        i = data.find(pattern, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if len(hits) > 1:
        fail("%s found %d times, expected once -- this is not the module this "
             "patch was written for" % (what, len(hits)))
    return hits[0] if hits else None


def module_state(path):
    """'stock', 'patched', or a reason it is neither."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return data, "not an ELF file"

    if find_once(data, ANCHOR, "the cap") is not None:
        return data, "stock"
    if find_once(data, ANCHOR_PATCHED, "the patched sequence") is not None:
        return data, "patched"
    return data, "neither the stock sequence nor a patched one is present"


def patch_module(path, revert):
    data, state = module_state(path)

    want = "patched" if revert else "stock"
    done = "stock" if revert else "patched"
    if state == done:
        print("  %s: already %s" % (MODULE, done))
        return False
    if state != want:
        fail("%s: %s" % (MODULE, state))

    src = ANCHOR_PATCHED if revert else ANCHOR
    dst = ANCHOR if revert else ANCHOR_PATCHED
    at = find_once(data, src, "the sequence")

    backup(path)
    patched = data[:at] + dst + data[at + len(dst):]
    with open(path, "wb") as f:
        f.write(patched)

    print("  %s: max_touch_number %d -> %d at file offset 0x%x"
          % (MODULE, MAX_TOUCH if revert else 1, 1 if revert else MAX_TOUCH,
             at + IMMEDIATE_OFFSET))
    return True


def patch_script(path, revert):
    """Keeps the insmod line honest. It changes no behaviour.

    cst_max_touch_number is declared as a module parameter and lands in .bss,
    but no instruction in the module ever reads it -- the driver uses the
    immediate patched above instead. The line is rewritten anyway so that the
    next person to read it is not told one thing by the script and another by
    the module.
    """
    with open(path, encoding="utf-8") as f:
        text = f.read()

    m = re.search(r"cst_max_touch_number=(\d+)", text)
    if not m:
        print("  %s: no cst_max_touch_number on the insmod line, left alone" % SCRIPT)
        return False

    current = int(m.group(1))
    wanted = 1 if revert else MAX_TOUCH
    if current == wanted:
        print("  %s: cst_max_touch_number is already %d" % (SCRIPT, wanted))
        return False

    backup(path)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text[:m.start(1)] + str(wanted) + text[m.end(1):])

    print("  %s: cst_max_touch_number %d -> %d (cosmetic: the module never "
          "reads it)" % (SCRIPT, current, wanted))
    return True


def backup(path):
    """Keeps the first version seen. A second run must not overwrite it."""
    dest = path + BACKUP_SUFFIX
    if not os.path.exists(dest):
        shutil.copy2(path, dest)
        print("  backup: %s" % os.path.basename(dest))


def check(module_path, script_path):
    _, state = module_state(module_path)
    print("  %s: %s" % (MODULE, state))

    with open(script_path, encoding="utf-8") as f:
        m = re.search(r"cst_max_touch_number=(\d+)", f.read())
    # Reported, never judged: the module does not read this number, so a stale
    # 1 here is untidy and nothing more.
    print("  %s: cst_max_touch_number=%s" % (SCRIPT, m.group(1) if m else "absent"))

    if state == "patched":
        print("multitouch: in place")
        return 0
    print("multitouch: NOT in place -- run this script without --check")
    return 1


def main():
    ap = argparse.ArgumentParser(description="Uncap the CST8xx touch driver.")
    ap.add_argument("directory", nargs="?", default=os.path.dirname(os.path.abspath(__file__)),
                    help="where the two files are (default: beside this script)")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="report the state and change nothing")
    group.add_argument("--revert", action="store_true", help="put the stock behaviour back")
    args = ap.parse_args()

    module_path = os.path.join(args.directory, MODULE)
    script_path = os.path.join(args.directory, SCRIPT)
    for path in (module_path, script_path):
        if not os.path.exists(path):
            fail("%s is not there" % path)

    if args.check:
        sys.exit(check(module_path, script_path))

    changed = patch_module(module_path, args.revert)
    changed |= patch_script(script_path, args.revert)

    if not changed:
        print("nothing to do")
        return

    if args.revert:
        print("reverted. Repack the firmware for it to take effect.")
    else:
        print("patched. Repack the firmware, flash it, and check with "
              "tools/mtcircles.c: MAX TOGETHER must reach 2.")


if __name__ == "__main__":
    main()
