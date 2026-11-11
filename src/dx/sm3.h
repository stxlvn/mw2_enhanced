#pragma once
// Direct3D 9 shader model 3 bytecode (vs_3_0 / ps_3_0) -> HLSL text for shader model 5 (docs/native-dx-plan.md). The C port of tools/sm3_to_hlsl.py, which is the reference: its output
// compiles to the same bytecode, and tools/sm3_semantic_check.py showed the translation computes what Direct3D 9 does on the shaders of nine levels.
#include <stddef.h>
#include <stdint.h>

// code: the shader's tokens (the version token first, up to the end token), count: their number. On success returns 1, *hlsl is a malloc'ed text (free it), *is_vs says which stage it is for
// (compile it with the entry point "main" and the target vs_5_0 or ps_5_0). On failure returns 0 and err holds the reason (a call / label subroutine, a predicated instruction, ...).
int sm3_translate(const uint32_t *code, size_t count, char **hlsl, int *is_vs, char *err, size_t err_size);
