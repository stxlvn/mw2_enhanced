#!/usr/bin/env python3
"""Byte signatures for the addresses the mod uses, so it can find them in a game build it has not seen (docs/address-resolution.md).

  python -I tools/addrsig.py gen x86 <reference image> <out file> [--addresses <file with hex rvas>]
  python -I tools/addrsig.py gen x64 <reference iw4sp.exe> <out file> [--objdump <llvm-objdump>]
  python -I tools/addrsig.py verify <signature file> <target image> [--truth <file of "ref rva -> rva" lines>]

A signature file (the format the game's loader in src/addr.c reads too) has one line per signature:

  CODE 0x00cf10 off=16 sig=8B 44 24 04 ?? ?? ?? ?? 56 ...     the function / code point at ref rva 0xcf10 is 16 bytes into the signature
  DATA 0x33ba88 abs  cap=4 sig=A1 ?? ?? ?? ?? ...               a global: the 4 bytes at offset 4 of the signature are its absolute address
  DATA 0x1b21e90 rip cap=3 tail=0 sig=48 8D 0D ?? ?? ?? ?? ...  a global: a rip-relative displacement at offset 3, `tail` bytes follow it in the instruction

?? are wildcards: absolute addresses, relative displacements and rip-relative displacements inside the window.
A signature counts only if it matches exactly one place in the target's code. An address is accepted when one candidate has at least
two votes (CODE with a single signature: one) and more than any other; otherwise it stays unresolved and the mod does not use it.
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ---------------------------------------------------------------------------------------------------------------------------- images


class Image:
    """A PE file or a flat memory dump: sections with their bytes, executable ones marked."""

    def __init__(self, path, arch, flat=False, flat_base=0x400000):
        self.arch = arch
        self.d = open(path, "rb").read()
        self.secs = []  # (name, va, vsize, raw offset, raw size, executable)
        if flat:
            self.base = flat_base
            self.secs = [(".text", flat_base + 0x1000, 0x293000, 0x1000, 0x293000, True)]
            self.flat = True
            return
        self.flat = False
        pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        n = struct.unpack_from("<H", self.d, pe + 6)[0]
        opt = struct.unpack_from("<H", self.d, pe + 20)[0]
        if arch == "x64":
            self.base = struct.unpack_from("<Q", self.d, pe + 24 + 24)[0]
        else:
            self.base = struct.unpack_from("<I", self.d, pe + 24 + 28)[0]
        sec = pe + 24 + opt
        for i in range(n):
            name, vs, va, rs, raw, _, _, _, _, ch = struct.unpack_from("<8sIIIIIIHHI", self.d, sec + 40 * i)
            self.secs.append((name.rstrip(b"\0").decode(), self.base + va, vs, raw, rs, bool(ch & 0x20000000)))

    def exec_sections(self):
        return [(va, self.d[raw:raw + min(vs, rs)]) for name, va, vs, raw, rs, ex in self.secs if ex]

    def text(self):
        """The one executable section the signatures are searched in (the first)."""
        return self.exec_sections()[0]

    def read(self, va, n):
        for name, sva, vs, raw, rs, ex in self.secs:
            if sva <= va < sva + max(vs, rs):
                off = raw + va - sva
                return self.d[off:off + n]
        return b""

    def is_code(self, va):
        return any(ex and sva <= va < sva + vs for name, sva, vs, raw, rs, ex in self.secs)


# ---------------------------------------------------------------------------------------------------------------------------- wildcard backends


class X86Backend:
    """32-bit: absolute addresses are 4-byte values inside the image, relative ones follow E8 / E9 / 0F 8x (the heuristics of map159.py)."""

    def __init__(self, img):
        import map159
        self.map159 = map159
        self.img = img
        self.lo, self.hi = map159.LO, map159.HI

    def wildmask(self, buf, va0, wild_at=()):
        return self.map159.wildmask(buf, va0, wild_at)

    def refs_to(self, data_va, text_va, text):
        needle = struct.pack("<I", data_va)
        out, i = [], text.find(needle)
        while i >= 0:
            out.append({"op": text_va + i, "kind": "abs", "tail": 0})
            i = text.find(needle, i + 1)
        return out


    def refs_near(self, data_va, text_va, text, span=(-0x20, 0x100)):
        """References to addresses close to data_va (a field of the table or struct that starts there): each carries delta = reference target - data_va."""
        if not hasattr(self, "_abs"):
            self._abs = {}
            for i in range(len(text) - 3):
                v = struct.unpack_from("<I", text, i)[0]
                if self.lo <= v < self.hi:
                    self._abs.setdefault(v, []).append(text_va + i)
            self._abs_keys = sorted(self._abs)
        import bisect
        out = []
        i = bisect.bisect_left(self._abs_keys, data_va + span[0])
        while i < len(self._abs_keys) and self._abs_keys[i] <= data_va + span[1]:
            v = self._abs_keys[i]
            if v != data_va:
                for op in self._abs[v][:4]:
                    out.append({"op": op, "kind": "abs", "tail": 0, "delta": v - data_va})
            i += 1
        out.sort(key=lambda r: abs(r["delta"]))
        return out


class X64Backend:
    """64-bit: the instruction boundaries and rip-relative targets come from llvm-objdump."""

    def __init__(self, img, objdump, exe_path):
        self.img = img
        self.fields = {}  # VA of a 4-byte relocatable field -> True (rip-relative disp32, call/jmp rel32)
        self.rip_refs = {}  # target VA -> [(field VA, tail)]
        text_va, text = img.text()
        out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--print-imm-hex", "--section=.text", exe_path],
                             capture_output=True, text=True, check=True).stdout
        prev = None  # (va, line) of the last instruction line
        instrs = []
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]{6,}):\s", line)
            if m:
                instrs.append([int(m.group(1), 16), None, line])
                continue
            m = re.match(r"\s+#\s+0x([0-9a-f]+)\s*$", line)
            if m and instrs:
                instrs[-1][1] = int(m.group(1), 16)
        for i, (va, target, line) in enumerate(instrs):
            nxt = instrs[i + 1][0] if i + 1 < len(instrs) else va + 1
            ln = nxt - va
            if ln < 5 or ln > 15:
                continue
            raw = text[va - text_va:va - text_va + ln]
            if target is None:
                m = re.search(r"\b(?:callq|jmpq|call|jmp|j[a-z]+)\s+0x([0-9a-f]+)", line)
                if not m:
                    continue
                target = int(m.group(1), 16)
            disp = struct.pack("<i", target - nxt) if -(1 << 31) <= target - nxt < (1 << 31) else None
            if disp is None:
                continue
            pos = raw.find(disp)
            if pos < 0:
                continue
            self.fields[va + pos] = True
            self.rip_refs.setdefault(target, []).append((va + pos, ln - pos - 4))

    def wildmask(self, buf, va0, wild_at=()):
        mark = [False] * len(buf)
        for p in wild_at:
            for k in range(4):
                if 0 <= p + k < len(buf):
                    mark[p + k] = True
        for i in range(len(buf) - 3):
            if (va0 + i) in self.fields:
                for k in range(4):
                    mark[i + k] = True
        return mark

    def refs_to(self, data_va, text_va, text):
        return [{"op": f, "kind": "rip", "tail": t} for f, t in self.rip_refs.get(data_va, [])]

    def refs_near(self, data_va, text_va, text, span=(-0x20, 0x100)):
        out = []
        for target in sorted(self.rip_refs):
            k = target - data_va
            if k == 0 or not (span[0] <= k <= span[1]):
                continue
            for f, t in self.rip_refs[target][:4]:
                out.append({"op": f, "kind": "rip", "tail": t, "delta": k})
        out.sort(key=lambda r: abs(r["delta"]))
        return out


# ---------------------------------------------------------------------------------------------------------------------------- signatures


def mask_to_sig(buf, mark):
    return " ".join("??" if w else "%02X" % b for b, w in zip(buf, mark))


def sig_to_pattern(sig):
    out = []
    for tok in sig.split():
        out.append(None if tok == "??" else int(tok, 16))
    return out


def find_all(text, pat, limit=3):
    """Offsets where pat (list of byte or None) matches text; stops after limit hits. Uses the longest fixed run as the anchor."""
    best, run_start, best_len = 0, 0, 0
    i = 0
    while i < len(pat):
        if pat[i] is None:
            i += 1
            continue
        j = i
        while j < len(pat) and pat[j] is not None:
            j += 1
        if j - i > best_len:
            best, best_len = i, j - i
        i = j
    if best_len < 3:
        return []
    anchor = bytes(pat[best:best + best_len])
    hits = []
    k = text.find(anchor)
    n = len(pat)
    while k >= 0:
        start = k - best
        if start >= 0 and start + n <= len(text):
            ok = True
            for idx, want in enumerate(pat):
                if want is not None and text[start + idx] != want:
                    ok = False
                    break
            if ok:
                hits.append(start)
                if len(hits) >= limit:
                    break
        k = text.find(anchor, k + 1)
    return hits


CODE_SHAPES = [(pre, post) for pre in (0, 8, 16, 32, 48, 64, 96, 128) for post in (12, 16, 24, 32, 48, 64, 96)]
DATA_SHAPES = [(pre, post) for pre in (4, 8, 12, 16, 24, 32) for post in (4, 8, 12, 16, 24, 32)]
MIN_FIXED = 12
DERIVE_REACH = 0x1000000  # a derived global lies this close to the resolved ones on both sides (the two nearest on each side must agree)
STRONG_FIXED = 40  # fixed bytes of one code signature that are enough on their own


def spread(sigs, count):
    """Up to count signatures chosen to differ: round-robin over the shapes' start offsets."""
    if len(sigs) <= count:
        return sigs
    step = len(sigs) / count
    return [sigs[int(i * step)] for i in range(count)]


def code_signatures(img, be, ref_rva, per_address=24):
    text_va, text = img.text()
    va = img.base + ref_rva
    found = []
    for pre, post in CODE_SHAPES:
        a0 = va - pre
        buf = img.read(a0, pre + post)
        if len(buf) < pre + post:
            continue
        mark = be.wildmask(buf, a0)
        if sum(1 for w in mark if not w) < MIN_FIXED:
            continue
        sig = mask_to_sig(buf, mark)
        if len(find_all(text, sig_to_pattern(sig), 2)) == 1:
            found.append(("CODE", ref_rva, "off=%d" % pre, sig))
    return spread(found, per_address)


def data_signatures(img, be, ref_rva, per_address=16, max_sites=60):
    text_va, text = img.text()
    va = img.base + ref_rva
    refs = be.refs_to(va, text_va, text)[:max_sites]
    if not refs:
        refs = be.refs_near(va, text_va, text)[:max_sites]
    found = []
    for r in refs:
        for pre, post in DATA_SHAPES:
            if r["kind"] == "abs":
                a0 = r["op"] - pre
                length = pre + 4 + post
                cap = pre
                tail = 0
            else:
                a0 = r["op"] - pre
                length = pre + 4 + r["tail"] + post
                cap = pre
                tail = r["tail"]
            buf = img.read(a0, length)
            if len(buf) < length:
                continue
            mark = be.wildmask(buf, a0, wild_at=(cap,))
            if sum(1 for w in mark if not w) < MIN_FIXED:
                continue
            sig = mask_to_sig(buf, mark)
            if len(find_all(text, sig_to_pattern(sig), 2)) == 1:
                extra = "abs cap=%d" % cap if r["kind"] == "abs" else "rip cap=%d tail=%d" % (cap, tail)
                if r.get("delta"):
                    extra += " delta=%d" % r["delta"]
                found.append(("DATA", ref_rva, extra, sig))
                break  # one shape per site; the sites give the votes
    return spread(found, per_address)


def write_sigs(path, arch, ref_desc, items, unresolved, img=None):
    lines = ["# mw2coop address signatures, %s. Made by tools/addrsig.py from %s." % (arch, ref_desc),
             "# See the header of that script for the format; docs/address-resolution.md for how the game uses this file."]
    if img is not None:  # the reference build's code, as RVAs: an address the file does not list is taken to be code or data by it (src/addr.c)
        for name, va, vs, raw, rs, ex in img.secs:
            if ex:
                lines.append("# code 0x%x 0x%x" % (va - img.base, va - img.base + vs))
    lines.append("")
    for kind, rva, extra, sig in items:
        lines.append("%s 0x%06x %s sig=%s" % (kind, rva, extra, sig))
    lines.append("")
    lines.append("# no signature could be made for these (a global next to resolved ones is derived from them):")
    for kind, rva in unresolved:
        lines.append("%s 0x%06x derive" % (kind, rva))
    open(path, "w", newline="\n").write("\n".join(lines) + "\n")


def parse_sigs(path):
    out = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(CODE|DATA)\s+0x([0-9a-f]+)\s+derive$", line)
        if m:
            out.append({"kind": m.group(1), "rva": int(m.group(2), 16), "derive": True})
            continue
        m = re.match(r"(CODE|DATA)\s+0x([0-9a-f]+)\s+(.*?)\s+sig=(.*)$", line)
        if not m:
            raise SystemExit("cannot read: " + line)
        kind, rva, extra, sig = m.groups()
        opts = dict(re.findall(r"(\w+)=(\d+)", extra))
        out.append({"kind": kind, "rva": int(rva, 16), "off": int(opts.get("off", 0)), "cap": int(opts.get("cap", 0)),
                    "tail": int(opts.get("tail", 0)), "delta": int(re.search(r"delta=(-?\d+)", extra).group(1)) if "delta=" in extra else 0, "mode": "rip" if "rip" in extra.split() else "abs", "pat": sig_to_pattern(sig)})
    return out


def resolve(sigs, tgt):
    """The accepted resolution of every address: {ref rva: target rva}. The rule the game's loader follows."""
    sections = tgt.exec_sections()
    votes = {}
    count = {}
    for s in sigs:
        if s.get("derive"):
            continue
        hits = []
        for va, text in sections:
            for h in find_all(text, s["pat"], 2):
                hits.append((va, text, h))
        count[s["rva"]] = count.get(s["rva"], 0) + 1
        if len(hits) != 1:
            continue
        va, text, h = hits[0]
        if s["kind"] == "CODE":
            cand = va + h + s["off"] - tgt.base
        elif s["mode"] == "abs":
            cand = struct.unpack_from("<I", text, h + s["cap"])[0] - tgt.base
        else:
            disp = struct.unpack_from("<i", text, h + s["cap"])[0]
            cand = va + h + s["cap"] + 4 + s["tail"] + disp - tgt.base
        cand -= s["delta"]
        votes.setdefault(s["rva"], {}).setdefault(cand, [0, 0])
        votes[s["rva"]][cand][0] += 1
        votes[s["rva"]][cand][1] = max(votes[s["rva"]][cand][1], sum(1 for b in s["pat"] if b is not None))
    result = {}
    for rva, v in votes.items():
        ranked = sorted(v.items(), key=lambda kv: (-kv[1][0], -kv[1][1]))
        top = ranked[0][1]
        is_code = any(s["kind"] == "CODE" for s in sigs if s["rva"] == rva)
        # two signatures agree, or (code only) one long signature: 40 fixed bytes do not match two places by chance
        strong = top[0] >= 2 or (is_code and top[1] >= STRONG_FIXED)
        if strong and (len(ranked) == 1 or ranked[0][1][0] > ranked[1][1][0]):
            result[rva] = ranked[0][0]
    # a global that no signature found: the globals around it moved as one block, so it moved by the same amount as the nearest resolved
    # ones below and above it, if those two agree and are close
    data = sorted(set(s["rva"] for s in sigs if s["kind"] == "DATA"))
    known = {r: v for r, v in result.items() if r in set(data)}
    for d in data:
        if d in result:
            continue
        below = [r for r in known if r < d and d - r <= DERIVE_REACH]
        above = [r for r in known if r > d and r - d <= DERIVE_REACH]
        if below and above:
            near = sorted(below)[-2:] + sorted(above)[:2]
            deltas = set(known[r] - r for r in near)
            if len(deltas) == 1:
                result[d] = d + deltas.pop()
    return result


# ---------------------------------------------------------------------------------------------------------------------------- command line


def load_addresses_x86(path):
    out = []
    if path:
        for line in open(path):
            m = re.search(r"0x([0-9a-f]+)", line)
            if m:
                out.append(int(m.group(1), 16))
        return sorted(set(out))
    src = open(os.path.join(HERE, "..", "src", "addr159.c"), encoding="utf-8").read()
    return sorted(set(int(m.group(1), 16) for m in re.finditer(r"\{(0x[0-9a-f]+), 0x[0-9a-f]+\}", src)))


def _strip_comments(t):
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def _code64(text):
    """The lines that only the 64-bit build compiles (inside #ifdef _WIN64 up to its #else, or #ifndef _WIN64's #else), comments stripped."""
    out, stack = [], []  # stack of [is_win64_condition, negated, in_else]
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
            out.append("")
            continue
        ok = True
        for win64, neg, els in stack:
            if win64 and not ((not neg and not els) or (neg and els)):
                ok = False
        out.append(_strip_comments(line) if ok else "")
    return "\n".join(out)


def _array_members(code, name, first_only):
    """The hex numbers of `name[] = {...};` (the first member of each {..} entry when first_only)."""
    res = []
    for m in re.finditer(r"\b%s\[[^\]]*\]\s*=\s*\{" % re.escape(name), code):
        i, depth, j = m.end(), 1, m.end()
        while j < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[j], 0)
            j += 1
        body = code[i:j - 1]
        if first_only:
            res += [int(h, 16) for h in re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,", body)]
        else:
            res += [int(h, 16) for h in re.findall(r"0x[0-9a-fA-F]+", body)]
    return res


def x64_addresses():
    """The 64-bit addresses the sources use: every XR(0x..) of the 64-bit code, the RVA_ macros of the 64-bit regions, and the tables the code walks (ranges, call sites,
    context hook sites, probes)."""
    out = {}
    for fn in sorted(os.listdir(os.path.join(HERE, "..", "src"))):
        if not fn.endswith((".c", ".h")) or fn in ("addr159.c",):
            continue
        text = open(os.path.join(HERE, "..", "src", fn), encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
        code = _code64(text)
        for m in re.finditer(r"\bXR\((0x[0-9a-fA-F]+)\)", code):
            out.setdefault(int(m.group(1), 16), fn)
        for m in re.finditer(r"#define\s+(?:RVA|REF)_[A-Z0-9_]+\s+(0x[0-9a-fA-F]+)", code):
            out.setdefault(int(m.group(1), 16), fn)
        for name, first in (("ranges", True), ("xranges", True), ("client_n_sites", True), ("sites", True), ("probes", False), ("net_port_immediates_official", False)):
            for v in _array_members(code, name, first):
                out.setdefault(v, fn + ":" + name)
    return out


def cmd_gen(argv):
    arch, ref, out = argv[0], argv[1], argv[2]
    opts = dict(zip(argv[3::2], argv[4::2]))
    if arch == "x86":
        img = Image(ref, "x86", flat=not ref.lower().endswith(".exe"))
        be = X86Backend(img)
        addrs = load_addresses_x86(opts.get("--addresses"))
    else:
        img = Image(ref, "x64")
        objdump = opts.get("--objdump") or os.path.join(HERE, "..", "..", "tools", "llvm-mingw", "llvm-mingw-20261006-ucrt-x86_64", "bin", "llvm-objdump.exe")
        be = X64Backend(img, objdump, ref)
        addrs = sorted(x64_addresses())
    items, missing = [], []
    for i, rva in enumerate(addrs):
        va = img.base + rva
        sigs = code_signatures(img, be, rva) if img.is_code(va) else data_signatures(img, be, rva)
        if not sigs:
            missing.append(("CODE" if img.is_code(va) else "DATA", rva))
        items += sigs
        if i % 25 == 0:
            print("%d / %d addresses" % (i, len(addrs)), flush=True)
    write_sigs(out, arch, os.path.basename(ref), items, missing, img)
    print("%d addresses, %d signatures, %d without a signature" % (len(addrs), len(items), len(missing)))


def cmd_verify(argv):
    sigfile, target = argv[0], argv[1]
    opts = dict(zip(argv[2::2], argv[3::2]))
    sigs = parse_sigs(sigfile)
    arch = "x64" if "x64" in open(sigfile).readline() else "x86"
    tgt = Image(target, arch, flat=not target.lower().endswith(".exe"))
    got = resolve(sigs, tgt)
    addrs = sorted(set(s["rva"] for s in sigs))
    print("%d addresses with signatures, %d resolved" % (len(addrs), len(got)))
    if "--truth" in opts:
        truth = {}
        for line in open(opts["--truth"]):
            m = re.match(r"\s*0x([0-9a-f]+)\s*->\s*0x([0-9a-f]+)", line)
            if m:
                truth[int(m.group(1), 16)] = int(m.group(2), 16)
        right = sum(1 for k, v in got.items() if truth.get(k) == v)
        wrong = [(k, v, truth[k]) for k, v in got.items() if k in truth and truth[k] != v]
        print("against the truth: %d right, %d wrong, %d of the truth not resolved" % (right, len(wrong), sum(1 for k in truth if k in set(addrs) and k not in got)))
        for k, v, t in wrong[:10]:
            print("  wrong: 0x%x -> 0x%x, truth 0x%x" % (k, v, t))
    return got


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("gen", "verify"):
        print(__doc__)
        sys.exit(2)
    {"gen": cmd_gen, "verify": cmd_verify}[sys.argv[1]](sys.argv[2:])
