// Direct3D 9 shader model 3 bytecode -> HLSL (shader model 5). The C port of tools/sm3_to_hlsl.py; read that file first, the structure is the same: a pass over the tokens that collects
// the declarations and the instructions, then an emitter that writes one HLSL statement per instruction with the register files as plain variables.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"

enum { OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3,
       OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC,
       OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI, OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEXLD = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81, OP_CMP = 88,
       OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96 };
enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_OUTPUT = 6, R_CONSTINT = 7, R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONSTBOOL = 14,
       R_LOOP = 15, R_MISC = 17 };

static const char *const USAGES[14] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
static const char *const CMP_OPS[8] = {"?", ">", "==", ">=", "<", "!=", "<=", "?"};

typedef struct {
	uint32_t num;
	int rtype, relative;
	uint32_t extra; // the address register token after a relatively addressed parameter
	uint32_t mask;
	int swizzle[4];
	uint32_t dmod, shift, smod;
	int is_dest;
} param_t;

typedef struct {
	int op, ctl, nops;
	param_t ops[8];
} instr_t;

typedef struct {
	int vs;
	float defs[256][4];
	uint8_t has_def[256];
	int32_t defi[16][4];
	uint8_t has_defi[16];
	uint8_t defb[16], has_defb[16];
	int in_used[16], in_usage[16], in_index[16];
	int out_used[16], out_usage[16], out_index[16];
	int sampler_used[16], sampler_kind[16];
	int maxtemp, maxcolor, uses_depth;
	int misc0, misc1;
	instr_t *body;
	size_t nbody, cap;
} shader_t;

// ---- a growing string
typedef struct {
	char *s;
	size_t n, cap;
} sb_t;

static void sb_add(sb_t *b, const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2 + 4096;
		b->s = realloc(b->s, b->cap);
	}
	memcpy(b->s + b->n, tmp, n);
	b->n += n;
	b->s[b->n] = 0;
}

typedef struct {
	char err[96];
} ctx_t;

#define FAIL(c, ...) do { snprintf((c)->err, sizeof((c)->err), __VA_ARGS__); return 0; } while (0)

static void param_decode(param_t *p, uint32_t tok)
{
	p->num = tok & 0x7ff;
	p->rtype = (int)(((tok >> 28) & 7) | (((tok >> 11) & 3) << 3));
	p->relative = (tok & 0x2000) != 0;
	p->mask = (tok >> 16) & 0xf;
	for (int i = 0; i < 4; i++)
		p->swizzle[i] = (int)((tok >> (16 + 2 * i)) & 3);
	p->dmod = (tok >> 20) & 0xf;
	p->shift = (tok >> 24) & 0xf;
	p->smod = (tok >> 24) & 0xf;
	p->extra = 0;
	p->is_dest = 0;
}

static int read_operand(const uint32_t *t, size_t count, size_t *i, param_t *p)
{
	if (*i >= count)
		return 0;
	param_decode(p, t[(*i)++]);
	if (p->relative) {
		if (*i >= count)
			return 0;
		p->extra = t[(*i)++];
	}
	return 1;
}

static void float_text(char *out, float f)
{
	if (!(f == f) || fabsf(f) == INFINITY) {
		strcpy(out, "0.0");
		return;
	}
	double d = (double)f;
	for (int prec = 1; prec <= 17; prec++) {
		snprintf(out, 40, "%.*g", prec, d);
		if (strtod(out, NULL) == d)
			break;
	}
	if (!strpbrk(out, ".eEn"))
		strcat(out, ".0");
}

static int parse(shader_t *sh, const uint32_t *t, size_t count, ctx_t *c)
{
	if (count < 2)
		FAIL(c, "size");
	const uint32_t ver = t[0];
	if ((ver >> 16) != 0xfffe && (ver >> 16) != 0xffff)
		FAIL(c, "version %08x", ver);
	if ((ver & 0xffff) != 0x300)
		FAIL(c, "version %08x", ver);
	sh->vs = (ver >> 16) == 0xfffe;
	sh->maxtemp = sh->maxcolor = -1;
	size_t i = 1;
	while (i < count) {
		const uint32_t tok = t[i];
		const int op = (int)(tok & 0xffff);
		if (op == 0xfffe) { // a comment (the constant table): skipped
			i += 1 + ((tok >> 16) & 0x7fff);
			continue;
		}
		if (op == 0xffff)
			break;
		const int length = (int)((tok >> 24) & 0xf);
		if (tok & 0x10000000)
			FAIL(c, "predicated instruction");
		const char *known = NULL;
		switch (op) {
		case OP_NOP: case OP_MOV: case OP_ADD: case OP_SUB: case OP_MAD: case OP_MUL: case OP_RCP: case OP_RSQ: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE:
		case OP_EXP: case OP_LOG: case OP_LIT: case OP_DST: case OP_LRP: case OP_FRC: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_CALL: case OP_CALLNZ:
		case OP_LOOP: case OP_RET: case OP_ENDLOOP: case OP_LABEL: case OP_DCL: case OP_POW: case OP_CRS: case OP_SGN: case OP_ABS: case OP_NRM: case OP_SINCOS: case OP_REP: case OP_ENDREP:
		case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_BREAK: case OP_BREAKC: case OP_MOVA: case OP_DEFB: case OP_DEFI: case OP_TEXCOORD: case OP_TEXKILL: case OP_TEXLD:
		case OP_EXPP: case OP_LOGP: case OP_CND: case OP_DEF: case OP_CMP: case OP_DP2ADD: case OP_DSX: case OP_DSY: case OP_TEXLDD: case OP_SETP: case OP_TEXLDL: case OP_BREAKP:
			known = "";
			break;
		}
		if (!known)
			FAIL(c, "opcode %d", op);
		size_t j = i + 1;
		const size_t end = i + 1 + (size_t)length;
		if (end > count)
			FAIL(c, "truncated");
		const int ctl = (int)((tok >> 16) & 0xff);
		if (op == OP_DCL) {
			param_t dst;
			if (j + 1 >= count)
				FAIL(c, "truncated");
			const uint32_t usage_tok = t[j];
			size_t k = j + 1;
			if (!read_operand(t, count, &k, &dst))
				FAIL(c, "truncated");
			const int usage = (int)(usage_tok & 0x1f), index = (int)((usage_tok >> 16) & 0xf);
			if (dst.num >= 16 && dst.rtype != R_MISC)
				FAIL(c, "register number");
			if (dst.rtype == R_SAMPLER) {
				sh->sampler_used[dst.num & 15] = 1;
				sh->sampler_kind[dst.num & 15] = (int)((usage_tok >> 27) & 0xf);
			} else if (dst.rtype == R_INPUT) {
				sh->in_used[dst.num] = 1;
				sh->in_usage[dst.num] = usage;
				sh->in_index[dst.num] = index;
			} else if (dst.rtype == R_OUTPUT) {
				sh->out_used[dst.num] = 1;
				sh->out_usage[dst.num] = usage;
				sh->out_index[dst.num] = index;
			} else if (dst.rtype == R_MISC) {
				if (dst.num == 0)
					sh->misc0 = 1;
				else if (dst.num == 1)
					sh->misc1 = 1;
			} else if (dst.rtype == R_COLOROUT || dst.rtype == R_DEPTHOUT) {
				// nothing to record
			} else {
				FAIL(c, "dcl register type %d", dst.rtype);
			}
		} else if (op == OP_DEF) {
			param_t dst;
			size_t k = j;
			if (!read_operand(t, count, &k, &dst) || k + 4 > count || dst.num > 255)
				FAIL(c, "def");
			memcpy(sh->defs[dst.num], &t[k], 16);
			sh->has_def[dst.num] = 1;
		} else if (op == OP_DEFI) {
			param_t dst;
			size_t k = j;
			if (!read_operand(t, count, &k, &dst) || k + 4 > count || dst.num > 15)
				FAIL(c, "defi");
			memcpy(sh->defi[dst.num], &t[k], 16);
			sh->has_defi[dst.num] = 1;
		} else if (op == OP_DEFB) {
			param_t dst;
			size_t k = j;
			if (!read_operand(t, count, &k, &dst) || k + 1 > count || dst.num > 15)
				FAIL(c, "defb");
			sh->defb[dst.num] = t[k] != 0;
			sh->has_defb[dst.num] = 1;
		} else {
			if (sh->nbody == sh->cap) {
				sh->cap = sh->cap ? sh->cap * 2 : 256;
				sh->body = realloc(sh->body, sh->cap * sizeof(instr_t));
			}
			instr_t *in = &sh->body[sh->nbody++];
			memset(in, 0, sizeof(*in));
			in->op = op;
			in->ctl = ctl;
			const int no_dest = op == OP_IF || op == OP_IFC || op == OP_ELSE || op == OP_ENDIF || op == OP_REP || op == OP_ENDREP || op == OP_ENDLOOP || op == OP_BREAK || op == OP_BREAKC ||
					    op == OP_RET || op == OP_NOP || op == OP_LOOP || op == OP_LABEL || op == OP_CALL || op == OP_CALLNZ || op == OP_BREAKP;
			int first = 1;
			while (j < end) {
				if (in->nops >= 8)
					FAIL(c, "too many operands");
				param_t *p = &in->ops[in->nops];
				if (!read_operand(t, count, &j, p))
					FAIL(c, "truncated");
				p->is_dest = first && !no_dest;
				first = 0;
				in->nops++;
				if (p->is_dest) {
					if (p->rtype == R_TEMP && (int)p->num > sh->maxtemp)
						sh->maxtemp = (int)p->num;
					if (p->rtype == R_COLOROUT && (int)p->num > sh->maxcolor)
						sh->maxcolor = (int)p->num;
					if (p->rtype == R_DEPTHOUT)
						sh->uses_depth = 1;
				} else if (p->rtype == R_TEMP && (int)p->num > sh->maxtemp) {
					sh->maxtemp = (int)p->num;
				}
			}
		}
		i = end;
	}
	return 1;
}

// ---- operands as HLSL text
static int reg_name(const shader_t *sh, const param_t *p, int extra, char *out, size_t sz, ctx_t *c)
{
	const int n = (int)p->num + extra;
	switch (p->rtype) {
	case R_TEMP:
		snprintf(out, sz, "r%d", n);
		return 1;
	case R_INPUT:
		snprintf(out, sz, "v%d", n);
		return 1;
	case R_CONST:
		if (p->relative) {
			const uint32_t e = p->extra;
			const int et = (int)(((e >> 28) & 7) | (((e >> 11) & 3) << 3));
			snprintf(out, sz, "c[%d + %s]", n, et == R_ADDR ? "a0.x" : "aL");
		} else if (n < 256 && sh->has_def[n]) {
			snprintf(out, sz, "c%d_def", n);
		} else {
			snprintf(out, sz, "c[%d]", n);
		}
		return 1;
	case R_ADDR:
		snprintf(out, sz, "a0");
		return 1;
	case R_OUTPUT:
		snprintf(out, sz, "o%d", n);
		return 1;
	case R_COLOROUT:
		snprintf(out, sz, "oC%d", n);
		return 1;
	case R_DEPTHOUT:
		snprintf(out, sz, "oDepth");
		return 1;
	case R_CONSTINT:
		snprintf(out, sz, n < 16 && sh->has_defi[n] ? "i%d_def" : "i%d", n);
		return 1;
	case R_CONSTBOOL:
		snprintf(out, sz, "b%d", n);
		return 1;
	case R_LOOP:
		snprintf(out, sz, "aL");
		return 1;
	case R_MISC:
		snprintf(out, sz, n == 0 ? "vPos" : "vFace");
		return 1;
	}
	FAIL(c, "register type %d", p->rtype);
}

static int src(const shader_t *sh, const param_t *p, int extra, char *out, size_t sz, ctx_t *c)
{
	char s[160], r[64];
	if (!reg_name(sh, p, extra, r, sizeof(r), c))
		return 0;
	char sw[5];
	for (int i = 0; i < 4; i++)
		sw[i] = "xyzw"[p->swizzle[i]];
	sw[4] = 0;
	if (strcmp(sw, "xyzw"))
		snprintf(s, sizeof(s), "%s.%s", r, sw);
	else
		snprintf(s, sizeof(s), "%s", r);
	char base[160]; // the part before the first '.', for the divide modifiers
	snprintf(base, sizeof(base), "%s", s);
	char *dot = strchr(base, '.');
	if (dot)
		*dot = 0;
	switch (p->smod) {
	case 0: snprintf(out, sz, "%s", s); break;
	case 1: snprintf(out, sz, "(-%s)", s); break;
	case 2: snprintf(out, sz, "(%s - 0.5)", s); break;
	case 3: snprintf(out, sz, "(-(%s - 0.5))", s); break;
	case 4: snprintf(out, sz, "(%s * 2.0 - 1.0)", s); break;
	case 5: snprintf(out, sz, "(-(%s * 2.0 - 1.0))", s); break;
	case 6: snprintf(out, sz, "(1.0 - %s)", s); break;
	case 7: snprintf(out, sz, "(%s * 2.0)", s); break;
	case 8: snprintf(out, sz, "(-(%s * 2.0))", s); break;
	case 9: snprintf(out, sz, "(%s / %s.z)", s, base); break;
	case 10: snprintf(out, sz, "(%s / %s.w)", s, base); break;
	case 11: snprintf(out, sz, "abs(%s)", s); break;
	case 12: snprintf(out, sz, "(-abs(%s))", s); break;
	default: FAIL(c, "source modifier %u", p->smod);
	}
	return 1;
}

static void mask_str(uint32_t m, char *out)
{
	int n = 0;
	for (int i = 0; i < 4; i++)
		if (m & (1u << i))
			out[n++] = "xyzw"[i];
	out[n] = 0;
}

static int assign(const shader_t *sh, const param_t *dst, const char *expr_in, char *out, size_t sz, ctx_t *c)
{
	char d[64], mk[8], expr[3000];
	if (!reg_name(sh, dst, 0, d, sizeof(d), c))
		return 0;
	mask_str(dst->mask, mk);
	if (dst->rtype == R_ADDR) {
		snprintf(out, sz, "%s.%s = ((int4)floor((%s) + 0.5)).%s;", d, mk, expr_in, mk);
		return 1;
	}
	snprintf(expr, sizeof(expr), "%s", expr_in);
	if (dst->dmod & 1) {
		char t[3000];
		snprintf(t, sizeof(t), "saturate(%s)", expr);
		snprintf(expr, sizeof(expr), "%s", t);
	}
	if (dst->shift) {
		const int sc = dst->shift > 7 ? (int)dst->shift - 16 : (int)dst->shift;
		static const char *const scale[16] = {"1.0", "2.0", "4.0", "8.0", "16.0", "32.0", "64.0", "128.0", "0.00390625", "0.0078125", "0.015625", "0.03125", "0.0625", "0.125", "0.25", "0.5"};
		char t[3000];
		snprintf(t, sizeof(t), "(%s) * %s", expr, scale[sc < 0 ? 16 + sc : sc]);
		snprintf(expr, sizeof(expr), "%s", t);
	}
	if (!strcmp(mk, "xyzw"))
		snprintf(out, sz, "%s = %s;", d, expr);
	else
		snprintf(out, sz, "%s.%s = (%s).%s;", d, mk, expr, mk);
	return 1;
}

typedef struct {
	int depth, n;
} flow_t;

static int emit_instr(const shader_t *sh, const instr_t *in, sb_t *L, flow_t *st, ctx_t *c)
{
	char S[4][700], X[4][720], a[3100], line[3200];
	const param_t *ops = in->ops;
	const int name = in->op;
	char ind[32] = "";
	for (int i = 0; i < 1 + st->depth && i < 30; i++)
		strcat(ind, "\t");
	// the source texts of operands 1..3 (the destination is operand 0)
	const int nsrc = in->nops;
#define NEEDS(k) do { if (nsrc <= (k)) FAIL(c, "operands of opcode %d", name); } while (0)
	for (int k = 0; k < 4 && k < nsrc; k++) {
		if (ops[k].is_dest || ops[k].rtype == R_SAMPLER)
			continue;
		if (!src(sh, &ops[k], 0, S[k], sizeof(S[k]), c))
			return 0;
		snprintf(X[k], sizeof(X[k]), "(%s).x", S[k]);
	}
#define D(expr) (assign(sh, &ops[0], (expr), line, sizeof(line), c) ? 1 : 0)
#define OUT_LINE() sb_add(L, "%s%s\n", ind, line)
	switch (name) {
	case OP_NOP:
		return 1;
	case OP_MOV:
	case OP_MOVA:
		NEEDS(1);
		if (!D(S[1]))
			return 0;
		OUT_LINE();
		return 1;
	case OP_ADD: NEEDS(2); snprintf(a, sizeof(a), "%s + %s", S[1], S[2]); break;
	case OP_SUB: NEEDS(2); snprintf(a, sizeof(a), "%s - %s", S[1], S[2]); break;
	case OP_MUL: NEEDS(2); snprintf(a, sizeof(a), "%s * %s", S[1], S[2]); break;
	case OP_MAD: NEEDS(3); snprintf(a, sizeof(a), "%s * %s + %s", S[1], S[2], S[3]); break;
	case OP_DP3: NEEDS(2); snprintf(a, sizeof(a), "(dot((%s).xyz, (%s).xyz)).xxxx", S[1], S[2]); break;
	case OP_DP4: NEEDS(2); snprintf(a, sizeof(a), "(dot(%s, %s)).xxxx", S[1], S[2]); break;
	case OP_DP2ADD: NEEDS(3); snprintf(a, sizeof(a), "(dot((%s).xy, (%s).xy) + %s).xxxx", S[1], S[2], X[3]); break;
	case OP_RCP: NEEDS(1); snprintf(a, sizeof(a), "(1.0 / %s).xxxx", X[1]); break;
	case OP_RSQ: NEEDS(1); snprintf(a, sizeof(a), "rsqrt(abs(%s)).xxxx", X[1]); break;
	case OP_MIN: NEEDS(2); snprintf(a, sizeof(a), "min(%s, %s)", S[1], S[2]); break;
	case OP_MAX: NEEDS(2); snprintf(a, sizeof(a), "max(%s, %s)", S[1], S[2]); break;
	case OP_SLT: NEEDS(2); snprintf(a, sizeof(a), "(float4)((%s) < (%s))", S[1], S[2]); break;
	case OP_SGE: NEEDS(2); snprintf(a, sizeof(a), "(float4)((%s) >= (%s))", S[1], S[2]); break;
	case OP_EXP:
	case OP_EXPP: NEEDS(1); snprintf(a, sizeof(a), "exp2(%s).xxxx", X[1]); break;
	case OP_LOG:
	case OP_LOGP: NEEDS(1); snprintf(a, sizeof(a), "log2(abs(%s)).xxxx", X[1]); break;
	case OP_POW: NEEDS(2); snprintf(a, sizeof(a), "pow(abs(%s), %s).xxxx", X[1], X[2]); break;
	case OP_LIT: {
		NEEDS(1);
		char inner[400];
		if (!D("float4(1.0, lx_, (s_.x > 0.0 && s_.y > 0.0) ? pow(s_.y, p_) : 0.0, 1.0)"))
			return 0;
		snprintf(inner, sizeof(inner), "%s", line);
		sb_add(L, "%s{ float4 s_ = %s; float lx_ = max(s_.x, 0.0), p_ = clamp(s_.w, -127.9961, 127.9961); %s }\n", ind, S[1], inner);
		return 1;
	}
	case OP_DST:
		NEEDS(2);
		snprintf(a, sizeof(a), "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", S[1], S[2], S[1], S[2]);
		break;
	case OP_LRP: NEEDS(3); snprintf(a, sizeof(a), "lerp(%s, %s, %s)", S[3], S[2], S[1]); break;
	case OP_FRC: NEEDS(1); snprintf(a, sizeof(a), "frac(%s)", S[1]); break;
	case OP_ABS: NEEDS(1); snprintf(a, sizeof(a), "abs(%s)", S[1]); break;
	case OP_SGN: NEEDS(1); snprintf(a, sizeof(a), "sign(%s)", S[1]); break;
	case OP_NRM: {
		NEEDS(1);
		if (!D("float4(s_.xyz * rsqrt(max(dot(s_.xyz, s_.xyz), 1e-30)), s_.w)"))
			return 0;
		char inner[400];
		snprintf(inner, sizeof(inner), "%s", line);
		sb_add(L, "%s{ float4 s_ = %s; %s }\n", ind, S[1], inner);
		return 1;
	}
	case OP_CRS: NEEDS(2); snprintf(a, sizeof(a), "float4(cross((%s).xyz, (%s).xyz), 0.0)", S[1], S[2]); break;
	case OP_SINCOS: {
		NEEDS(1);
		if (!D("float4(cos(s_), sin(s_), 0.0, 0.0)"))
			return 0;
		char inner[400];
		snprintf(inner, sizeof(inner), "%s", line);
		sb_add(L, "%s{ float s_ = %s; %s }\n", ind, X[1], inner);
		return 1;
	}
	case OP_CMP: NEEDS(3); snprintf(a, sizeof(a), "((%s) >= 0.0) ? (%s) : (%s)", S[1], S[2], S[3]); break;
	case OP_DSX: NEEDS(1); snprintf(a, sizeof(a), "ddx(%s)", S[1]); break;
	case OP_DSY: NEEDS(1); snprintf(a, sizeof(a), "ddy(%s)", S[1]); break;
	case OP_M4x4:
	case OP_M4x3:
	case OP_M3x4:
	case OP_M3x3:
	case OP_M3x2: {
		NEEDS(2);
		int rows = 4, cols = 4;
		if (name == OP_M4x3) { rows = 3; cols = 4; }
		else if (name == OP_M3x4) { rows = 4; cols = 3; }
		else if (name == OP_M3x3) { rows = 3; cols = 3; }
		else if (name == OP_M3x2) { rows = 2; cols = 3; }
		char parts[4][400], joined[1700] = "";
		const char *sel = cols == 4 ? "xyzw" : "xyz";
		for (int r = 0; r < rows; r++) {
			char m[160];
			if (!src(sh, &ops[2], r, m, sizeof(m), c))
				return 0;
			snprintf(parts[r], sizeof(parts[r]), "dot((%s).%s, (%s).%s)", S[1], sel, m, sel);
		}
		for (int r = 0; r < 4; r++) {
			if (r)
				strcat(joined, ", ");
			strcat(joined, r < rows ? parts[r] : "0.0");
		}
		snprintf(a, sizeof(a), "float4(%s)", joined);
		break;
	}
	case OP_TEXLD:
	case OP_TEXLDL:
	case OP_TEXLDD: {
		NEEDS(2);
		const int s_n = (int)ops[2].num & 15;
		const int kind = sh->sampler_used[s_n] ? sh->sampler_kind[s_n] : 2;
		const char *dim = kind == 3 || kind == 4 ? ".xyz" : ".xy";
		char tex[16], ss[16];
		snprintf(tex, sizeof(tex), "tex_s%d", s_n);
		snprintf(ss, sizeof(ss), "smp_s%d", s_n);
		if (name == OP_TEXLDL) {
			snprintf(a, sizeof(a), "%s.SampleLevel(%s, (%s)%s, (%s).w)", tex, ss, S[1], dim, S[1]);
		} else if (name == OP_TEXLDD) {
			NEEDS(4);
			char s4[700];
			if (!src(sh, &ops[4], 0, s4, sizeof(s4), c))
				return 0;
			snprintf(a, sizeof(a), "%s.SampleGrad(%s, (%s)%s, (%s)%s, (%s)%s)", tex, ss, S[1], dim, S[3], dim, s4, dim);
		} else if (in->ctl & 1) {
			snprintf(a, sizeof(a), "%s.Sample(%s, (%s)%s / (%s).w)", tex, ss, S[1], dim, S[1]);
		} else if (in->ctl & 2) {
			snprintf(a, sizeof(a), "%s.SampleBias(%s, (%s)%s, (%s).w)", tex, ss, S[1], dim, S[1]);
		} else {
			snprintf(a, sizeof(a), "%s.Sample(%s, (%s)%s)", tex, ss, S[1], dim);
		}
		break;
	}
	case OP_TEXKILL: {
		char r[64];
		if (!reg_name(sh, &ops[0], 0, r, sizeof(r), c))
			return 0;
		sb_add(L, "%sclip((%s).xyz);\n", ind, r);
		return 1;
	}
	case OP_IF: {
		const param_t *p = &ops[0];
		if (p->rtype == R_CONSTBOOL && p->num < 16 && sh->has_defb[p->num])
			sb_add(L, "%sif (%s) {\n", ind, sh->defb[p->num] ? "true" : "false");
		else if (p->rtype == R_CONSTBOOL)
			sb_add(L, "%sif (B(%u)) {\n", ind, p->num);
		else
			sb_add(L, "%sif (false) {\n", ind);
		st->depth++;
		return 1;
	}
	case OP_IFC: {
		NEEDS(1);
		char s0[700], s1[700];
		if (!src(sh, &ops[0], 0, s0, sizeof(s0), c) || !src(sh, &ops[1], 0, s1, sizeof(s1), c))
			return 0;
		sb_add(L, "%sif ((%s).x %s (%s).x) {\n", ind, s0, CMP_OPS[in->ctl & 7], s1);
		st->depth++;
		return 1;
	}
	case OP_ELSE: {
		char i2[32] = "";
		for (int i = 0; i < st->depth && i < 30; i++)
			strcat(i2, "\t");
		sb_add(L, "%s} else {\n", i2);
		return 1;
	}
	case OP_ENDIF:
	case OP_ENDREP:
	case OP_ENDLOOP: {
		st->depth--;
		char i2[32] = "";
		for (int i = 0; i < 1 + st->depth && i < 30; i++)
			strcat(i2, "\t");
		sb_add(L, "%s}\n", i2);
		return 1;
	}
	case OP_REP: {
		char r[64];
		if (!reg_name(sh, &ops[0], 0, r, sizeof(r), c))
			return 0;
		st->n++;
		sb_add(L, "%sfor (int rc%d = 0; rc%d < %s.x; rc%d++) {\n", ind, st->n, st->n, r, st->n);
		st->depth++;
		return 1;
	}
	case OP_LOOP: {
		NEEDS(1);
		char r[64];
		if (!reg_name(sh, &ops[1], 0, r, sizeof(r), c))
			return 0;
		st->n++;
		sb_add(L, "%sfor (int lc%d = 0; lc%d < %s.x; lc%d++) { aL = %s.y + lc%d * %s.z;\n", ind, st->n, st->n, r, st->n, r, st->n, r);
		st->depth++;
		return 1;
	}
	case OP_BREAK:
		sb_add(L, "%sbreak;\n", ind);
		return 1;
	case OP_BREAKC: {
		NEEDS(1);
		char s0[700], s1[700];
		if (!src(sh, &ops[0], 0, s0, sizeof(s0), c) || !src(sh, &ops[1], 0, s1, sizeof(s1), c))
			return 0;
		sb_add(L, "%sif ((%s).x %s (%s).x) break;\n", ind, s0, CMP_OPS[in->ctl & 7], s1);
		return 1;
	}
	case OP_RET:
		return 1;
	default:
		FAIL(c, "opcode %d", name);
	}
	if (!D(a))
		return 0;
	OUT_LINE();
	return 1;
#undef D
#undef OUT_LINE
#undef NEEDS
}

static const char *usage_name(int u)
{
	return u >= 0 && u < 14 ? USAGES[u] : "TEXCOORD";
}

int sm3_translate_vs(const uint32_t *code, size_t count, int flags, const sm3_int_input *ints, int nints, char **hlsl, int *is_vs, char *err, size_t err_size)
{
	shader_t *sh = calloc(1, sizeof(*sh));
	ctx_t c = {{0}};
	sb_t L = {0};
	int ok = 0;
	if (!parse(sh, code, count, &c))
		goto done;
	sb_add(&L, "// translated from Direct3D 9 shader model 3 (src/dx/sm3.c)\n");
	sb_add(&L, "cbuffer ConstF : register(b0) { float4 c[256]; };\n");
	sb_add(&L, "cbuffer ConstIB : register(b1) { int4 ic[16]; uint4 bc[8]; };\n");
	sb_add(&L, "#define B(n) (bc[(n) >> 2][(n) & 3] != 0)\n");
	const int alpha_test = (flags & SM3_ALPHA_TEST) && !sh->vs && sh->maxcolor >= 0;
	if (alpha_test)
		sb_add(&L, "cbuffer AlphaTestCB : register(b2) { float4 at; };\n");
	for (int n = 0; n < 16; n++) {
		if (!sh->sampler_used[n])
			continue;
		const int k = sh->sampler_kind[n];
		sb_add(&L, "%s tex_s%d : register(t%d); SamplerState smp_s%d : register(s%d);\n", k == 3 ? "TextureCube" : k == 4 ? "Texture3D" : "Texture2D", n, n, n, n);
	}
	sb_t ins = {0};
	int in_kind[16] = {0}; // integer vertex inputs (vertex shader only), see sm3_int_input
	for (int n = 0; n < 16; n++)
		if (sh->in_used[n] && sh->vs)
			for (int q = 0; q < nints; q++)
				if (ints[q].usage == sh->in_usage[n] && ints[q].index == sh->in_index[n])
					in_kind[n] = ints[q].kind;
	for (int n = 0; n < 16; n++)
		if (sh->in_used[n])
			sb_add(&ins, "\t%s v%d : %s%d;\n", in_kind[n] == 1 ? "uint4" : in_kind[n] == 2 ? "int4" : "float4", n, usage_name(sh->in_usage[n]), sh->in_index[n]);
	if (sh->vs) {
		sb_add(&L, "struct VSIn {\n%s\n};\n", ins.s ? ins.s : "");
		// the outputs: the position first, then by register
		sb_t outs = {0};
		for (int pass = 0; pass < 2; pass++)
			for (int n = 0; n < 16; n++) {
				if (!sh->out_used[n])
					continue;
				const int is_pos = sh->out_usage[n] == 0 && sh->out_index[n] == 0;
				if ((pass == 0) != is_pos)
					continue;
				if (is_pos)
					sb_add(&outs, "\tfloat4 o%d : SV_Position;\n", n);
				else
					sb_add(&outs, "\tfloat4 o%d : %s%d;\n", n, usage_name(sh->out_usage[n]), sh->out_index[n]);
			}
		sb_add(&L, "struct VSOut {\n%s\n};\n", outs.s ? outs.s : "");
		free(outs.s);
		sb_add(&L, "VSOut main(VSIn input)\n{\n\tVSOut output;\n");
		for (int n = 0; n < 16; n++)
			if (sh->in_used[n])
				sb_add(&L, in_kind[n] ? "\tfloat4 v%d = float4(input.v%d);\n" : "\tfloat4 v%d = input.v%d;\n", n, n);
		for (int n = 0; n < 16; n++)
			if (sh->out_used[n])
				sb_add(&L, "\tfloat4 o%d = 0;\n", n);
	} else {
		sb_t ps = {0};
		sb_add(&ps, "\tfloat4 vPosIn : SV_Position;\n");
		if (ins.s)
			sb_add(&ps, "%s", ins.s);
		if (sh->misc1)
			sb_add(&ps, "\tbool isFront : SV_IsFrontFace;\n");
		sb_add(&L, "struct PSIn {\n%s\n};\n", ps.s);
		free(ps.s);
		sb_t outs = {0};
		for (int n = 0; n <= sh->maxcolor; n++)
			sb_add(&outs, "\tfloat4 oC%d : SV_Target%d;\n", n, n);
		if (sh->uses_depth)
			sb_add(&outs, "\tfloat oDepth : SV_Depth;\n");
		sb_add(&L, "struct PSOut {\n%s\n};\n", outs.s ? outs.s : "");
		free(outs.s);
		sb_add(&L, "PSOut main(PSIn input)\n{\n\tPSOut output;\n");
		for (int n = 0; n < 16; n++)
			if (sh->in_used[n])
				sb_add(&L, "\tfloat4 v%d = input.v%d;\n", n, n);
		if (sh->misc0)
			sb_add(&L, "\tfloat4 vPos = input.vPosIn;\n");
		if (sh->misc1)
			sb_add(&L, "\tfloat4 vFace = input.isFront ? 1.0 : -1.0;\n");
		for (int n = 0; n <= sh->maxcolor; n++)
			sb_add(&L, "\tfloat4 oC%d = 0;\n", n);
		if (sh->uses_depth)
			sb_add(&L, "\tfloat oDepth = 0;\n");
	}
	free(ins.s);
	for (int n = 0; n <= sh->maxtemp; n++)
		sb_add(&L, "\tfloat4 r%d = 0;\n", n);
	sb_add(&L, "\tint4 a0 = 0; int aL = 0;\n");
	for (int n = 0; n < 256; n++) {
		if (!sh->has_def[n])
			continue;
		char f[4][48];
		for (int k = 0; k < 4; k++)
			float_text(f[k], sh->defs[n][k]);
		sb_add(&L, "\tconst float4 c%d_def = float4(%s, %s, %s, %s);\n", n, f[0], f[1], f[2], f[3]);
	}
	for (int n = 0; n < 16; n++)
		if (sh->has_defi[n])
			sb_add(&L, "\tconst int4 i%d_def = int4(%d, %d, %d, %d);\n", n, sh->defi[n][0], sh->defi[n][1], sh->defi[n][2], sh->defi[n][3]);
	flow_t st = {0, 0};
	for (size_t i = 0; i < sh->nbody; i++)
		if (!emit_instr(sh, &sh->body[i], &L, &st, &c))
			goto done;
	if (sh->vs) {
		for (int n = 0; n < 16; n++)
			if (sh->out_used[n])
				sb_add(&L, "\toutput.o%d = o%d;\n", n, n);
	} else {
		if (alpha_test) { // Direct3D 9's alpha test, on the 8-bit alpha of render target 0 (at = {D3DCMP function, reference 0..255, enable, 0})
			sb_add(&L, "\tif (at.z > 0.5) {\n\t\tfloat a_ = round(saturate(oC0.a) * 255.0); int f_ = (int)at.x; bool ok_ = true;\n");
			sb_add(&L, "\t\tif (f_ == 1) ok_ = false; else if (f_ == 2) ok_ = a_ < at.y; else if (f_ == 3) ok_ = a_ == at.y; else if (f_ == 4) ok_ = a_ <= at.y;\n");
			sb_add(&L, "\t\telse if (f_ == 5) ok_ = a_ > at.y; else if (f_ == 6) ok_ = a_ != at.y; else if (f_ == 7) ok_ = a_ >= at.y;\n");
			sb_add(&L, "\t\tclip(ok_ ? 1.0 : -1.0);\n\t}\n");
		}
		for (int n = 0; n <= sh->maxcolor; n++)
			sb_add(&L, "\toutput.oC%d = oC%d;\n", n, n);
		if (sh->uses_depth)
			sb_add(&L, "\toutput.oDepth = oDepth;\n");
	}
	sb_add(&L, "\treturn output;\n}\n");
	*hlsl = L.s;
	L.s = NULL;
	*is_vs = sh->vs;
	ok = 1;
done:
	if (!ok && err && err_size) {
		snprintf(err, err_size, "%s", c.err);
	}
	free(L.s);
	free(sh->body);
	free(sh);
	return ok;
}

int sm3_translate_ex(const uint32_t *code, size_t count, int flags, char **hlsl, int *is_vs, char *err, size_t err_size)
{
	return sm3_translate_vs(code, count, flags, NULL, 0, hlsl, is_vs, err, err_size);
}

int sm3_translate(const uint32_t *code, size_t count, char **hlsl, int *is_vs, char *err, size_t err_size)
{
	return sm3_translate_ex(code, count, 0, hlsl, is_vs, err, err_size);
}
