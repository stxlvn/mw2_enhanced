#!/usr/bin/env python3
"""Does the HLSL that tools/sm3_to_hlsl.py makes compute what the Direct3D 9 shader computes? (docs/native-dx-plan.md, step 2)

  python -I tools/sm3_semantic_check.py <folder of ps_*.bin> [limit] [work folder]

Each pixel shader of the folder (the files MW2COOP_DUMPSHADERS=1 writes) is run by tools/sm3check.c on a Direct3D 9 device (the original bytecode) and on a Direct3D 11 WARP device (its
translation, compiled to ps_5_0) with the same constants, interpolants and textures, and the pixel they write is compared. The harness is built here with the llvm-mingw compiler of the
workspace (../tools/llvm-mingw). Shaders with a depth output are skipped; samplers get a 2D, cube or volume texture by their declaration.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sm3_to_hlsl as m  # noqa: E402


def blob_bytes(text, target, flags=0):
    import ctypes
    lib = ctypes.WinDLL("d3dcompiler_47.dll")
    code = ctypes.c_void_p()
    err = ctypes.c_void_p()
    src = text.encode("ascii", "replace")
    hr = lib.D3DCompile(src, ctypes.c_size_t(len(src)), b"sm3", None, None, b"main", target.encode(), ctypes.c_uint(flags), ctypes.c_uint(0), ctypes.byref(code), ctypes.byref(err))
    if hr < 0 or not code.value:
        return None
    vt = ctypes.cast(ctypes.cast(code, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(code)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(code)
    return ctypes.string_at(ptr, size)


def vs_sources(sh):
    """The vertex shaders that write the same constants to every interpolant the pixel shader reads (shader model 3 for Direct3D 9, 5 for Direct3D 11)."""
    fields3, fields5, body = [], [], []
    for k, (n, (u, idx)) in enumerate(sorted(sh.inputs.items())):
        sem = "%s%d" % (m.USAGES.get(u, "TEXCOORD"), idx)
        if u == 0:
            raise m.Unsupported("a position interpolant")
        fields3.append("\tfloat4 f%d : %s;" % (n, sem))
        fields5.append("\tfloat4 f%d : %s;" % (n, sem))
        body.append("\to.f%d = vc[%d];" % (n, k % 16))
    v3 = "float4 vc[16] : register(c0);\nstruct VO {\n\tfloat4 pos : POSITION;\n%s\n};\nVO main(float4 p : POSITION)\n{\n\tVO o;\n\to.pos = p;\n%s\n\treturn o;\n}\n" % ("\n".join(fields3), "\n".join(body))
    v5 = "cbuffer C : register(b0) { float4 vc[16]; };\nstruct VO {\n\tfloat4 pos : SV_Position;\n%s\n};\nVO main(float4 p : POSITION)\n{\n\tVO o;\n\to.pos = p;\n%s\n\treturn o;\n}\n" % ("\n".join(fields5), "\n".join(body))
    return v3, v5


def check_vertex(folder, limit, work, exe):
    """The vertex shaders (without sampler reads): Direct3D 9 software vertex processing against the translation on WARP with stream output."""
    names = sorted(f for f in os.listdir(folder) if f.startswith("vs_") and f.endswith(".bin"))
    lines = []
    skipped = {}
    for f in names:
        if len(lines) >= limit:
            break
        data = open(os.path.join(folder, f), "rb").read()
        try:
            sh = m.Shader(data)
            if sh.samplers:
                raise m.Unsupported("a texture read in the vertex shader")
            target, text = m.translate(data)
            cso = blob_bytes(text, target)
            if cso is None:
                raise m.Unsupported("the translation does not compile")
        except m.Unsupported as e:
            skipped[str(e)] = skipped.get(str(e), 0) + 1
            continue
        base = f[:-4]
        open(os.path.join(work, base + ".vcso"), "wb").write(cso)
        ins = ",".join("%d:%d" % sh.inputs[n] for n in sorted(sh.inputs))
        outs = ",".join("%d:%d" % sh.outputs[n] for n in sorted(sh.outputs))
        if not ins or not outs:
            skipped["no inputs or no outputs"] = skipped.get("no inputs or no outputs", 0) + 1
            continue
        lines.append("%s %s %d %s %s %s" % (os.path.join(folder, f), os.path.join(work, base + ".vcso"), int(base[3:], 16) & 0xffff, base, ins, outs))
    manifest = os.path.join(work, "manifest_vs.txt")
    open(manifest, "w").write("\n".join(lines).replace("\\", "/") + "\n")
    r = subprocess.run([exe, "--vs", manifest], capture_output=True, text=True, timeout=3600)
    res = [l for l in r.stdout.splitlines() if l.strip()]
    tally = {}
    for l in res:
        parts = l.split()
        tally[parts[1] if len(parts) > 1 else "?"] = tally.get(parts[1] if len(parts) > 1 else "?", 0) + 1
    print("%d vertex shaders tried (skipped: %s)" % (len(lines), ", ".join("%s %d" % (k, v) for k, v in skipped.items()) or "none"))
    print("results:", ", ".join("%s %d" % (k, v) for k, v in sorted(tally.items())))
    for l in [x for x in res if " MATCH" not in x][:25]:
        print("  " + l[:220])
    if r.returncode:
        print("the harness stopped with %d; %s" % (r.returncode, r.stderr.strip()[:300]))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 10 ** 9
    work = sys.argv[3] if len(sys.argv) > 3 else os.path.join(folder, "..", "sm3work")
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(work, "sm3check.exe")
    cc = os.path.join(HERE, "..", "..", "tools", "llvm-mingw", "llvm-mingw-20261006-ucrt-x86_64", "bin", "x86_64-w64-mingw32-clang")
    src = os.path.join(HERE, "sm3check.c")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        r = subprocess.run([cc, "-O2", "-o", exe, src, "-ld3d9", "-ld3d11", "-ldxguid", "-lm"], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            return 1
    if os.environ.get("SM3_VS"):
        check_vertex(folder, limit, work, exe)
        return 0
    names = sorted(f for f in os.listdir(folder) if f.startswith("ps_") and f.endswith(".bin"))
    skipped = {}
    lines = []
    for f in names:
        if len(lines) >= limit:
            break
        data = open(os.path.join(folder, f), "rb").read()
        try:
            sh = m.Shader(data)
            if sh.uses_depth:
                raise m.Unsupported("depth output")
            if any(u == 0 for (u, _) in sh.inputs.values()):
                raise m.Unsupported("a position interpolant")
            target, text = m.translate(data)
            cso = blob_bytes(text, target)
            if cso is None:
                raise m.Unsupported("the translation does not compile")
            v3, v5 = vs_sources(sh)
            vs30 = blob_bytes(v3, "vs_3_0")
            vs50 = blob_bytes(v5, "vs_5_0")
            if vs30 is None or vs50 is None:
                raise m.Unsupported("the vertex shader of the test does not compile")
        except m.Unsupported as e:
            skipped[str(e)] = skipped.get(str(e), 0) + 1
            continue
        base = f[:-4]
        open(os.path.join(work, base + ".cso"), "wb").write(cso)
        open(os.path.join(work, base + ".vs30"), "wb").write(vs30)
        open(os.path.join(work, base + ".vs50"), "wb").write(vs50)
        kinds = "".join(str(sh.samplers.get(i, 2)) for i in range(16))  # 2 two-dimensional, 3 cube, 4 volume
        lines.append("%s %s %s %s %d %d %s %s" % (os.path.join(folder, f), os.path.join(work, base + ".cso"), os.path.join(work, base + ".vs30"), os.path.join(work, base + ".vs50"),
                                                    int(base[3:], 16) & 0xffff, max(1, sh.maxcolor + 1), base, kinds))
    manifest = os.path.join(work, "manifest.txt")
    open(manifest, "w").write("\n".join(lines).replace("\\", "/") + "\n")
    r = subprocess.run([exe, manifest], capture_output=True, text=True, timeout=3600)
    res = [l for l in r.stdout.splitlines() if l.strip()]
    tally = {}
    for l in res:
        parts = l.split()
        key = parts[1] if len(parts) > 1 else "?"
        tally[key] = tally.get(key, 0) + 1
    print("%d pixel shaders tried (skipped: %s)" % (len(lines), ", ".join("%s %d" % (k, v) for k, v in skipped.items()) or "none"))
    print("results:", ", ".join("%s %d" % (k, v) for k, v in sorted(tally.items())))
    diffs = []
    for l in res:
        parts = l.split()
        if len(parts) > 2 and parts[1] == "DIFF":
            try:
                diffs.append((float(parts[2]), l))
            except ValueError:
                pass
    if diffs:  # a difference of a few hundredths is the hardware's (the Direct3D 9 GPU's approximations, 8-bit texture filter weights) against WARP's exact arithmetic; larger ones are looked at
        print("differences beyond the tolerance: %d below 0.02, %d from 0.02 to 0.2, %d above 0.2" % (sum(1 for d, _ in diffs if d < 0.02), sum(1 for d, _ in diffs if 0.02 <= d < 0.2), sum(1 for d, _ in diffs if d >= 0.2)))
        for d, l in sorted(diffs, reverse=True)[:25]:
            print("  " + l[:230])
    for l in res:
        if " ERR" in l:
            print("  " + l)
    if r.returncode:
        print("the harness stopped with %d after %d results; %s" % (r.returncode, len(res), r.stderr.strip()[:300]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
