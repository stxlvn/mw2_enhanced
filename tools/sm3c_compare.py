#!/usr/bin/env python3
"""Compares the C translator (src/dx/sm3.c, run by sm3c_test) with the Python one (tools/sm3_to_hlsl.py) on a folder of dumped shaders.

  python -I tools/sm3c_compare.py <shader folder> <folder with the C output (<name>.bin.hlsl)>

A shader counts as equal when the two texts are identical, or when both compile (d3dcompiler_47, optimisation off, no debug info) to the same bytecode. Prints the totals and the first differences.
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm3_to_hlsl as py  # noqa: E402


def blob(text, target):
    lib = ctypes.WinDLL("d3dcompiler_47.dll")
    code = ctypes.c_void_p()
    err = ctypes.c_void_p()
    b = text.encode("ascii", "replace")
    hr = lib.D3DCompile(b, ctypes.c_size_t(len(b)), None, None, None, b"main", target.encode(), ctypes.c_uint((1 << 11) | (1 << 14)), ctypes.c_uint(0), ctypes.byref(code), ctypes.byref(err))
    if hr < 0 or not code.value:
        return None
    vt = ctypes.cast(ctypes.cast(code, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(code)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(code)
    return ctypes.string_at(ptr, size)


def main():
    folder, cdir = sys.argv[1], sys.argv[2]
    names = sorted(f for f in os.listdir(folder) if f.endswith(".bin") and f[:3] in ("vs_", "ps_"))
    same_text = same_code = differ = missing = 0
    shown = 0
    for f in names:
        cpath = os.path.join(cdir, f + ".hlsl")
        try:
            target, ptext = py.translate(open(os.path.join(folder, f), "rb").read())
        except py.Unsupported:
            ptext = None
        if not os.path.exists(cpath):
            if ptext is not None:
                missing += 1
                print("only Python translates:", f)
            continue
        ctext = open(cpath, "rb").read().decode("ascii", "replace")
        if ptext is None:
            differ += 1
            print("only C translates:", f)
            continue
        if ctext == ptext:
            same_text += 1
            continue
        a, b = blob(ptext, target), blob(ctext, target)
        if a is not None and a == b:
            same_code += 1
            continue
        differ += 1
        if shown < 5:
            shown += 1
            pl, cl = ptext.splitlines(), ctext.splitlines()
            print("DIFF", f, "compiled:", a is not None, b is not None)
            for i in range(max(len(pl), len(cl))):
                x = pl[i] if i < len(pl) else "<none>"
                y = cl[i] if i < len(cl) else "<none>"
                if x != y:
                    print("  py:", x[:200])
                    print("  c :", y[:200])
                    break
    print("%d shaders: %d identical text, %d same bytecode, %d differ, %d missing in C" % (len(names), same_text, same_code, differ, missing))


main()
