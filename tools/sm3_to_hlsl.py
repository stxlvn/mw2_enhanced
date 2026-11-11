#!/usr/bin/env python3
"""Direct3D 9 shader model 3 bytecode (vs_3_0 / ps_3_0) -> HLSL text for shader model 5.

The first piece of a native Direct3D 11 / 12 backend (docs/native-dx-plan.md): the game ships its shaders as D3D9 bytecode and D3D11/12 cannot load it.

  python -I tools/sm3_to_hlsl.py <file.bin>            print the HLSL of one shader (the file is what MW2COOP_DUMPSHADERS=1 writes)
  python -I tools/sm3_to_hlsl.py --check <dir>         translate and compile (d3dcompiler_47) every vs_*.bin / ps_*.bin of a folder and print the totals and the commonest failures

The register files become plain variables: r# temporaries, c# constants as one float4 array `c` (a cbuffer, register b0), i# and b# constants in a second cbuffer (b1), inputs v# and outputs o# /
oC# by their declared usage (the semantic names of the vertex declaration and the interpolants stay, so input layouts and the vertex -> pixel matching work as in Direct3D 9). `def` constants are
literals. What is not translated (call/label subroutines, predication, the ps_1 texture ops) raises Unsupported and is counted. Texture samplers become a Texture2D / TextureCube / Texture3D and
a SamplerState each (t#, s#); whether a depth texture is sampled with a comparison (Direct3D 9's hardware shadow maps) is a property of the bound texture, not of the shader, and is left to the backend.
"""
import ctypes
import os
import struct
import sys
from collections import Counter

OPS = {0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log",
       16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop",
       30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else", 43: "endif",
       44: "break", 45: "breakc", 46: "mova", 47: "defb", 48: "defi", 64: "texcoord", 65: "texkill", 66: "texld", 78: "expp", 79: "logp", 80: "cnd", 81: "def", 88: "cmp",
       90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl", 96: "breakp"}
USAGES = {0: "POSITION", 1: "BLENDWEIGHT", 2: "BLENDINDICES", 3: "NORMAL", 4: "PSIZE", 5: "TEXCOORD", 6: "TANGENT", 7: "BINORMAL", 8: "TESSFACTOR", 9: "POSITIONT", 10: "COLOR",
          11: "FOG", 12: "DEPTH", 13: "SAMPLE"}
R_TEMP, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_OUTPUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT, R_SAMPLER = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
R_CONSTBOOL, R_LOOP, R_MISC, R_LABEL, R_PREDICATE = 14, 15, 17, 18, 19
CMP_OPS = {1: ">", 2: "==", 3: ">=", 4: "<", 5: "!=", 6: "<="}
SWZ = "xyzw"


class Unsupported(Exception):
    pass


class Param:
    def __init__(self, tok, extra=None):
        self.num = tok & 0x7ff
        self.rtype = ((tok >> 28) & 7) | (((tok >> 11) & 3) << 3)
        self.relative = bool(tok & 0x2000)
        self.mask = (tok >> 16) & 0xf  # destination: write mask; source: swizzle in bits 16-23
        self.swizzle = [(tok >> (16 + 2 * i)) & 3 for i in range(4)]
        self.dmod = (tok >> 20) & 0xf  # destination: result modifier
        self.shift = (tok >> 24) & 0xf  # destination: shift scale (signed 4 bits)
        self.smod = (tok >> 24) & 0xf  # source: source modifier
        self.extra = extra  # the address register token that follows a relatively addressed parameter


class Shader:
    def __init__(self, data):
        if len(data) % 4:
            raise Unsupported("size")
        self.t = struct.unpack("<%dI" % (len(data) // 4), data)
        ver = self.t[0]
        if ver >> 16 not in (0xfffe, 0xffff) or (ver & 0xffff) != 0x300:
            raise Unsupported("version %08x" % ver)
        self.vs = (ver >> 16) == 0xfffe
        self.defs = {}  # c# -> 4 floats
        self.defi = {}
        self.defb = {}
        self.inputs = {}  # v# -> (usage, index)
        self.outputs = {}  # o# -> (usage, index)
        self.samplers = {}  # s# -> type
        self.maxtemp = -1
        self.maxcolor = -1
        self.misc = set()
        self.body = []
        self.uses_depth = False
        self.rel_const = False
        self.parse()

    def operand(self, i):
        tok = self.t[i]
        i += 1
        extra = None
        if tok & 0x2000:  # relative addressing: the address register's token follows
            extra = self.t[i]
            i += 1
        return Param(tok, extra), i

    def parse(self):
        t = self.t
        i = 1
        depth = 0
        loops = []
        while i < len(t):
            tok = t[i]
            op = tok & 0xffff
            if op == 0xfffe:  # comment (CTAB): skipped
                i += 1 + ((tok >> 16) & 0x7fff)
                continue
            if op == 0xffff:
                break
            length = (tok >> 24) & 0xf
            if tok & 0x10000000:
                raise Unsupported("predicated instruction")
            name = OPS.get(op)
            if name is None:
                raise Unsupported("opcode %d" % op)
            j = i + 1
            end = i + 1 + length
            ctl = (tok >> 16) & 0xff
            if name == "dcl":
                usage_tok = t[j]
                dst, _ = self.operand(j + 1)
                usage = usage_tok & 0x1f
                index = (usage_tok >> 16) & 0xf
                if dst.rtype == R_SAMPLER:
                    self.samplers[dst.num] = (usage_tok >> 27) & 0xf
                elif dst.rtype == R_INPUT:
                    self.inputs[dst.num] = (usage, index)
                elif dst.rtype == R_OUTPUT:
                    self.outputs[dst.num] = (usage, index)
                elif dst.rtype == R_MISC:
                    self.misc.add(dst.num)
                elif dst.rtype in (R_COLOROUT, R_DEPTHOUT):
                    pass
                else:
                    raise Unsupported("dcl register type %d" % dst.rtype)
            elif name == "def":
                dst, k = self.operand(j)
                self.defs[dst.num] = struct.unpack("<4f", struct.pack("<4I", *t[k:k + 4]))
            elif name == "defi":
                dst, k = self.operand(j)
                self.defi[dst.num] = struct.unpack("<4i", struct.pack("<4I", *t[k:k + 4]))
            elif name == "defb":
                dst, k = self.operand(j)
                self.defb[dst.num] = t[k] != 0
            else:
                self.body.append((name, ctl, self.parse_operands(name, j, end)))
            i = end
        # nothing more to prepare: the temporaries and outputs are found by the emitter

    def parse_operands(self, name, j, end):
        ops = []
        t = self.t
        no_dest = name in ("if", "ifc", "else", "endif", "rep", "endrep", "endloop", "break", "breakc", "ret", "nop", "loop", "label", "call", "callnz", "breakp")
        first = True
        while j < end:
            p, j = self.operand(j)
            p.is_dest = first and not no_dest
            first = False
            ops.append(p)
            if p.is_dest:
                if p.rtype == R_TEMP:
                    self.maxtemp = max(self.maxtemp, p.num)
                if p.rtype == R_COLOROUT:
                    self.maxcolor = max(self.maxcolor, p.num)
                if p.rtype == R_DEPTHOUT:
                    self.uses_depth = True
            elif p.rtype == R_TEMP:
                self.maxtemp = max(self.maxtemp, p.num)
        return ops


def reg_name(sh, p, matrix_extra=0):
    n = p.num + matrix_extra
    if p.rtype == R_TEMP:
        return "r%d" % n
    if p.rtype == R_INPUT:
        return "v%d" % n
    if p.rtype == R_CONST:
        if p.relative:
            sh.rel_const = True
            idx = "a0.x" if p.extra is not None and ((p.extra >> 28) & 7) | (((p.extra >> 11) & 3) << 3) == R_ADDR else "aL"
            return "c[%d + %s]" % (n, idx)
        if n in sh.defs:
            return "c%d_def" % n
        return "c[%d]" % n
    if p.rtype == R_ADDR:
        return "a0"
    if p.rtype == R_OUTPUT:
        return "o%d" % n
    if p.rtype == R_COLOROUT:
        return "oC%d" % n
    if p.rtype == R_DEPTHOUT:
        return "oDepth"
    if p.rtype == R_CONSTINT:
        return "i%d" % n if n not in sh.defi else "i%d_def" % n
    if p.rtype == R_CONSTBOOL:
        return "b%d" % n
    if p.rtype == R_LOOP:
        return "aL"
    if p.rtype == R_MISC:
        return "vPos" if n == 0 else "vFace"
    raise Unsupported("register type %d" % p.rtype)


def src(sh, p, matrix_extra=0):
    s = reg_name(sh, p, matrix_extra)
    sw = "".join(SWZ[x] for x in p.swizzle)
    if sw != "xyzw":
        s = "%s.%s" % (s, sw)
    m = p.smod
    if m == 0:
        return s
    if m == 1:
        return "(-%s)" % s
    if m == 2:
        return "(%s - 0.5)" % s
    if m == 3:
        return "(-(%s - 0.5))" % s
    if m == 4:
        return "(%s * 2.0 - 1.0)" % s
    if m == 5:
        return "(-(%s * 2.0 - 1.0))" % s
    if m == 6:
        return "(1.0 - %s)" % s
    if m == 7:
        return "(%s * 2.0)" % s
    if m == 8:
        return "(-(%s * 2.0))" % s
    if m == 9:
        return "(%s / %s.z)" % (s, s.split(".")[0] if "." in s else s)
    if m == 10:
        return "(%s / %s.w)" % (s, s.split(".")[0] if "." in s else s)
    if m == 11:
        return "abs(%s)" % s
    if m == 12:
        return "(-abs(%s))" % s
    raise Unsupported("source modifier %d" % m)


def mask_str(m):
    return "".join(SWZ[i] for i in range(4) if m & (1 << i))


def assign(sh, dst, expr):
    """dst = expr, the expression being a float4 (a value for every component); the result modifier and the shift apply, the write mask picks the components."""
    d = reg_name(sh, dst)
    if dst.rtype == R_ADDR:
        mk = mask_str(dst.mask)
        return "%s.%s = (int4)floor((%s) + 0.5).%s;" % (d, mk, expr, mk) if False else "%s.%s = ((int4)floor((%s) + 0.5)).%s;" % (d, mk, expr, mk)
    if dst.dmod & 1:
        expr = "saturate(%s)" % expr
    sh_ = dst.shift
    if sh_:
        sc = sh_ - 16 if sh_ > 7 else sh_
        expr = "(%s) * %s" % (expr, repr(float(2 ** sc)))
    mk = mask_str(dst.mask)
    if mk == "xyzw":
        return "%s = %s;" % (d, expr)
    return "%s.%s = (%s).%s;" % (d, mk, expr, mk)


def emit_instr(sh, name, ctl, ops, lines, state):
    def S(i, extra=0):
        return src(sh, ops[i], extra)

    def D(expr):
        return assign(sh, ops[0], expr)

    def sx(i):
        return "(%s).x" % S(i)

    ind = "\t" * (1 + state["depth"])
    out = lines.append
    if name == "nop":
        return
    if name == "mov":
        out(ind + D(S(1)))
    elif name == "mova":
        out(ind + D(S(1)))
    elif name == "add":
        out(ind + D("%s + %s" % (S(1), S(2))))
    elif name == "sub":
        out(ind + D("%s - %s" % (S(1), S(2))))
    elif name == "mul":
        out(ind + D("%s * %s" % (S(1), S(2))))
    elif name == "mad":
        out(ind + D("%s * %s + %s" % (S(1), S(2), S(3))))
    elif name == "dp3":
        out(ind + D("(%s).xxxx" % ("dot((%s).xyz, (%s).xyz)" % (S(1), S(2)))))
    elif name == "dp4":
        out(ind + D("(%s).xxxx" % ("dot(%s, %s)" % (S(1), S(2)))))
    elif name == "dp2add":
        out(ind + D("(%s).xxxx" % ("dot((%s).xy, (%s).xy) + %s" % (S(1), S(2), sx(3)))))
    elif name == "rcp":
        out(ind + D("(1.0 / %s).xxxx" % sx(1)))
    elif name == "rsq":
        out(ind + D("rsqrt(abs(%s)).xxxx" % sx(1)))
    elif name == "min":
        out(ind + D("min(%s, %s)" % (S(1), S(2))))
    elif name == "max":
        out(ind + D("max(%s, %s)" % (S(1), S(2))))
    elif name == "slt":
        out(ind + D("(float4)((%s) < (%s))" % (S(1), S(2))))
    elif name == "sge":
        out(ind + D("(float4)((%s) >= (%s))" % (S(1), S(2))))
    elif name in ("exp", "expp"):
        out(ind + D("exp2(%s).xxxx" % sx(1)))
    elif name in ("log", "logp"):
        out(ind + D("log2(abs(%s)).xxxx" % sx(1)))
    elif name == "pow":
        out(ind + D("pow(abs(%s), %s).xxxx" % (sx(1), sx(2))))
    elif name == "lit":
        out(ind + "{ float4 s_ = %s; float lx_ = max(s_.x, 0.0), p_ = clamp(s_.w, -127.9961, 127.9961); %s }" % (
            S(1), D("float4(1.0, lx_, (s_.x > 0.0 && s_.y > 0.0) ? pow(s_.y, p_) : 0.0, 1.0)")))
    elif name == "dst":
        out(ind + D("float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)" % (S(1), S(2), S(1), S(2))))
    elif name == "lrp":
        out(ind + D("lerp(%s, %s, %s)" % (S(3), S(2), S(1))))
    elif name == "frc":
        out(ind + D("frac(%s)" % S(1)))
    elif name == "abs":
        out(ind + D("abs(%s)" % S(1)))
    elif name == "sgn":
        out(ind + D("sign(%s)" % S(1)))
    elif name == "nrm":
        out(ind + "{ float4 s_ = %s; %s }" % (S(1), D("float4(normalize(s_.xyz), 0.0)") if False else D("float4(s_.xyz * rsqrt(max(dot(s_.xyz, s_.xyz), 1e-30)), s_.w)")))
    elif name == "crs":
        out(ind + D("float4(cross((%s).xyz, (%s).xyz), 0.0)" % (S(1), S(2))))
    elif name == "sincos":
        out(ind + "{ float s_ = %s; %s }" % (sx(1), D("float4(cos(s_), sin(s_), 0.0, 0.0)")))
    elif name == "cmp":
        out(ind + D("((%s) >= 0.0) ? (%s) : (%s)" % (S(1), S(2), S(3))))
    elif name == "dsx":
        out(ind + D("ddx(%s)" % S(1)))
    elif name == "dsy":
        out(ind + D("ddy(%s)" % S(1)))
    elif name in ("m4x4", "m4x3", "m3x4", "m3x3", "m3x2"):
        rows, cols = {"m4x4": (4, 4), "m4x3": (3, 4), "m3x4": (4, 3), "m3x3": (3, 3), "m3x2": (2, 3)}[name]
        v = S(1)
        parts = ["dot((%s).%s, %s)" % (v, "xyzw"[:cols], src(sh, ops[2], r)) if False else "dot((%s).%s, (%s).%s)" % (v, "xyzw"[:cols], src(sh, ops[2], r), "xyzw"[:cols]) for r in range(rows)]
        parts += ["0.0"] * (4 - rows)
        out(ind + D("float4(%s)" % ", ".join(parts)))
    elif name in ("texld", "texldl", "texldd"):
        smp = ops[2] if name != "texldd" else ops[2]
        s_n = smp.num
        kind = sh.samplers.get(s_n, 2)
        co = S(1)
        dim = {2: ".xy", 3: ".xyz", 4: ".xyz"}.get(kind, ".xy")
        tex, ss = "tex_s%d" % s_n, "smp_s%d" % s_n
        if name == "texldl":
            e = "%s.SampleLevel(%s, (%s)%s, (%s).w)" % (tex, ss, co, dim, co)
        elif name == "texldd":
            e = "%s.SampleGrad(%s, (%s)%s, (%s)%s, (%s)%s)" % (tex, ss, co, dim, S(3), dim, S(4), dim)
        elif ctl & 1:  # texldp: projected
            e = "%s.Sample(%s, (%s)%s / (%s).w)" % (tex, ss, co, dim, co)
        elif ctl & 2:  # texldb: biased
            e = "%s.SampleBias(%s, (%s)%s, (%s).w)" % (tex, ss, co, dim, co)
        else:
            e = "%s.Sample(%s, (%s)%s)" % (tex, ss, co, dim)
        out(ind + D(e))
    elif name == "texkill":
        out(ind + "clip((%s).xyz);" % reg_name(sh, ops[0]))
    elif name == "if":
        out(ind + "if (%s) {" % ("B(%d)" % ops[0].num if ops[0].rtype == R_CONSTBOOL and ops[0].num not in sh.defb else str(sh.defb.get(ops[0].num, False)).lower()))
        state["depth"] += 1
    elif name == "ifc":
        out(ind + "if ((%s).x %s (%s).x) {" % (S(0), CMP_OPS[ctl & 7], S(1)))
        state["depth"] += 1
    elif name == "else":
        out("\t" * state["depth"] + "} else {")
    elif name == "endif" or name == "endrep" or name == "endloop":
        state["depth"] -= 1
        out("\t" * (1 + state["depth"]) + "}")
    elif name == "rep":
        state["n"] += 1
        out(ind + "for (int rc%d = 0; rc%d < %s.x; rc%d++) {" % (state["n"], state["n"], reg_name(sh, ops[0]), state["n"]))
        state["depth"] += 1
    elif name == "loop":
        state["n"] += 1
        n = state["n"]
        ireg = reg_name(sh, ops[1])
        out(ind + "for (int lc%d = 0; lc%d < %s.x; lc%d++) { aL = %s.y + lc%d * %s.z;" % (n, n, ireg, n, ireg, n, ireg))
        state["depth"] += 1
    elif name == "break":
        out(ind + "break;")
    elif name == "breakc":
        out(ind + "if ((%s).x %s (%s).x) break;" % (S(0), CMP_OPS[ctl & 7], S(1)))
    elif name == "ret":
        pass
    else:
        raise Unsupported(name)


def translate(data):
    sh = Shader(data)
    L = []
    L.append("// translated from Direct3D 9 shader model 3 (tools/sm3_to_hlsl.py)")
    L.append("cbuffer ConstF : register(b0) { float4 c[256]; };")
    L.append("cbuffer ConstIB : register(b1) { int4 ic[16]; uint4 bc[8]; };")
    L.append("#define B(n) (bc[(n) >> 2][(n) & 3] != 0)")
    for n, kind in sorted(sh.samplers.items()):
        tt = {2: "Texture2D", 3: "TextureCube", 4: "Texture3D"}.get(kind, "Texture2D")
        L.append("%s tex_s%d : register(t%d); SamplerState smp_s%d : register(s%d);" % (tt, n, n, n, n))
    # inputs and outputs
    ins = ["\t%s v%d : %s%d;" % ("float4", n, USAGES.get(u, "TEXCOORD"), idx) for n, (u, idx) in sorted(sh.inputs.items())]
    if sh.vs:
        outs = []
        for n, (u, idx) in sorted(sh.outputs.items(), key=lambda kv: (not (kv[1][0] == 0 and kv[1][1] == 0), kv[0])):
            sem = "SV_Position" if u == 0 and idx == 0 else "%s%d" % (USAGES.get(u, "TEXCOORD"), idx)
            outs.append("\tfloat4 o%d : %s;" % (n, sem))
        L.append("struct VSIn {\n%s\n};" % "\n".join(ins))
        L.append("struct VSOut {\n%s\n};" % "\n".join(outs))
        L.append("VSOut main(VSIn input)\n{")
        L.append("\tVSOut output;")
        for n in sh.inputs:
            L.append("\tfloat4 v%d = input.v%d;" % (n, n))
        for n in sorted(sh.outputs):
            L.append("\tfloat4 o%d = 0;" % n)
    else:
        ins_ps = ["\tfloat4 vPosIn : SV_Position;"] + list(ins)  # always: Direct3D 11 links the stages by register and the vertex shader's output starts with the position
        if 1 in sh.misc:
            ins_ps.append("\tbool isFront : SV_IsFrontFace;")
        L.append("struct PSIn {\n%s\n};" % "\n".join(ins_ps))
        outs = ["\tfloat4 oC%d : SV_Target%d;" % (n, n) for n in range(sh.maxcolor + 1)]
        if sh.uses_depth:
            outs.append("\tfloat oDepth : SV_Depth;")
        L.append("struct PSOut {\n%s\n};" % "\n".join(outs))
        L.append("PSOut main(PSIn input)\n{")
        L.append("\tPSOut output;")
        for n in sh.inputs:
            L.append("\tfloat4 v%d = input.v%d;" % (n, n))
        if 0 in sh.misc:
            L.append("\tfloat4 vPos = input.vPosIn;")
        if 1 in sh.misc:
            L.append("\tfloat4 vFace = input.isFront ? 1.0 : -1.0;")
        for n in range(sh.maxcolor + 1):
            L.append("\tfloat4 oC%d = 0;" % n)
        if sh.uses_depth:
            L.append("\tfloat oDepth = 0;")
    for n in range(sh.maxtemp + 1):
        L.append("\tfloat4 r%d = 0;" % n)
    L.append("\tint4 a0 = 0; int aL = 0;")
    for n, v in sorted(sh.defs.items()):
        L.append("\tconst float4 c%d_def = float4(%s);" % (n, ", ".join(repr(float(x)) if x == x and abs(x) != float("inf") else "0.0" for x in v)))
    for n, v in sorted(sh.defi.items()):
        L.append("\tconst int4 i%d_def = int4(%s);" % (n, ", ".join(str(x) for x in v)))
    state = {"depth": 0, "n": 0}
    for name, ctl, ops in sh.body:
        emit_instr(sh, name, ctl, ops, L, state)
    if sh.vs:
        for n in sorted(sh.outputs):
            L.append("\toutput.o%d = o%d;" % (n, n))
        L.append("\treturn output;")
    else:
        for n in range(sh.maxcolor + 1):
            L.append("\toutput.oC%d = oC%d;" % (n, n))
        if sh.uses_depth:
            L.append("\toutput.oDepth = oDepth;")
        L.append("\treturn output;")
    L.append("}")
    return ("vs_5_0" if sh.vs else "ps_5_0"), "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------------------- compiling (Windows: d3dcompiler_47.dll)
def compile_hlsl(text, target):
    lib = ctypes.WinDLL("d3dcompiler_47.dll")
    code = ctypes.c_void_p()
    err = ctypes.c_void_p()
    src_b = text.encode("ascii", "replace")
    hr = lib.D3DCompile(src_b, ctypes.c_size_t(len(src_b)), b"sm3", None, None, b"main", target.encode(), ctypes.c_uint(1 << 11), ctypes.c_uint(0), ctypes.byref(code), ctypes.byref(err))
    msg = ""
    if err.value:
        vt = ctypes.cast(ctypes.cast(err, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(err)
        size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(err)
        msg = ctypes.string_at(ptr, size).decode("ascii", "replace")
    return hr >= 0, msg


def check(folder):
    names = sorted(f for f in os.listdir(folder) if f.endswith(".bin") and f[:3] in ("vs_", "ps_"))
    ok = bad_translate = bad_compile = 0
    why = Counter()
    example = {}
    for f in names:
        data = open(os.path.join(folder, f), "rb").read()
        try:
            target, text = translate(data)
        except Unsupported as e:
            bad_translate += 1
            why["translate: %s" % e] += 1
            continue
        except Exception as e:  # a bug of the translator: counted, not hidden
            bad_translate += 1
            why["translator error: %s" % type(e).__name__] += 1
            example.setdefault("translator error: %s" % type(e).__name__, f)
            continue
        good, msg = compile_hlsl(text, target)
        if good:
            ok += 1
        else:
            bad_compile += 1
            first = next((l for l in msg.splitlines() if "error" in l), msg[:100])
            key = "compile: " + first.split("): ", 1)[-1][:110]
            why[key] += 1
            example.setdefault(key, f)
    print("%d shaders: %d translated and compiled, %d not translated, %d translated but not compiled" % (len(names), ok, bad_translate, bad_compile))
    for k, n in why.most_common(25):
        print("  %5d  %s   (e.g. %s)" % (n, k, example.get(k, "")))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        check(sys.argv[2])
    elif len(sys.argv) == 2:
        tg, txt = translate(open(sys.argv[1], "rb").read())
        print("// target " + tg)
        print(txt)
    else:
        print(__doc__)
