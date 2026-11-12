#pragma once
// FSR 1 (EASU + RCAS) for the native renderers: the pixel shaders are the Direct3D 9 ones of upscale.c (ports of AMD's FidelityFX FSR 1, MIT license, see there) rewritten to shader model 5 by a few
// text substitutions: the texture and sampler declarations, the constants in a constant buffer b0, and the pixel position from SV_Position.
#include <stdlib.h>
#include <string.h>
#include "../upscale.h"

static char *nu_replace(char *s, const char *from, const char *to)
{
	char *at = strstr(s, from);
	if (!at)
		return s;
	const size_t fl = strlen(from), tl = strlen(to), n = strlen(s);
	char *o = malloc(n - fl + tl + 1);
	memcpy(o, s, (size_t)(at - s));
	memcpy(o + (at - s), to, tl);
	memcpy(o + (at - s) + tl, at + fl, n - (size_t)(at - s) - fl + 1);
	free(s);
	return o;
}

// the SM5 text of EASU (rcas = 0) or RCAS (rcas = 1); free() it
static char *nu_shader(int rcas)
{
	char *s = _strdup(rcas ? upscale_rcas_hlsl : upscale_easu_hlsl);
	s = nu_replace(s, "sampler2D src : register(s0);\n", "Texture2D tx : register(t0);\nSamplerState sm : register(s0);\n#define tex2D(s, uv) tx.SampleLevel(sm, uv, 0)\n#define vpos (sv.xy)\n");
	if (rcas) {
		s = nu_replace(s, "float4 k0 : register(c0);\n", "cbuffer K : register(b0) { float4 k0; };\n");
	} else {
		s = nu_replace(s, "float4 c0 : register(c0);\n", "cbuffer K : register(b0) { float4 c0; float4 c1; float4 c2; float4 c3; };\n");
		s = nu_replace(s, "float4 c1 : register(c1);\n", "");
		s = nu_replace(s, "float4 c2 : register(c2);\n", "");
		s = nu_replace(s, "float4 c3 : register(c3);\n", "");
	}
	s = nu_replace(s, "float4 main(float2 vpos : VPOS) : COLOR", "float4 main(float4 sv : SV_Position) : SV_Target");
	return s;
}
