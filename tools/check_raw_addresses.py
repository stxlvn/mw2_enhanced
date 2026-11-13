#!/usr/bin/env python3
"""Fails when 64-bit code of the mod uses a raw offset into the game's exe instead of XR() (docs/address-resolution.md).

  python -I tools/check_raw_addresses.py

A build whose addresses moved is only handled if every address the 64-bit code uses goes through XR() (the resolver); a literal such as `base + 0x1bab60` would still point at the reference
build's place. The check reads src/*.c and src/*.h (src/dx is the renderer, guarded as a whole by its build hash: src/dx/natsites_gen.h; src/x86 is 32-bit) and looks at the lines the 64-bit build
compiles for `<base> + 0x<4 or more hex digits>` and `<base>[0x...]`. A line that has a good reason carries `raw-ok: <why>` in a comment (image bounds, a scan window). Run by tools/pr_checks.py.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
PAT = re.compile(r"\b(?:base|exe|image_base|exe_base)\s*\)?\s*[+\[]\s*0x[0-9a-fA-F]{4,}")
SKIP = ("addr159.c", "settings_template_gen.h")


def lines64(text):
    out, stack = [], []
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if m:
            k, rest = m.group(1), m.group(2)
            if k in ("ifdef", "ifndef", "if"):
                neg = k == "ifndef" or "!defined(_WIN64)" in rest
                stack.append(["_WIN64" in rest, neg, False])
            elif k in ("elif", "else"):
                stack[-1][2] = True
            elif k == "endif":
                stack.pop()
            out.append(False)
            continue
        only32 = any(is64 and ((not els) == neg) for is64, neg, els in stack)  # the branch only the 32-bit build compiles
        out.append(not only32)
    return out


def main():
    bad = []
    for path in sorted(glob.glob(os.path.join(SRC, "*.c")) + glob.glob(os.path.join(SRC, "*.h"))):
        name = os.path.basename(path)
        if name in SKIP:
            continue
        text = open(path, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
        flags = lines64(text)
        for n, line in enumerate(text.split("\n"), 1):
            if not flags[n - 1] or "raw-ok" in line:
                continue
            code = line.split("//")[0]
            if PAT.search(code):
                bad.append("%s:%d: %s" % (name, n, line.strip()[:130]))
    if bad:
        print("raw offsets into the exe in 64-bit code (use XR(), or say why with `raw-ok: ...` in a comment):")
        for b in bad:
            print("  " + b)
        return 1
    print("no raw exe offsets in the 64-bit code of src/*.c, src/*.h")
    return 0


sys.exit(main())
