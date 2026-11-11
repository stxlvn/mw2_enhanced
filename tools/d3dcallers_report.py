#!/usr/bin/env python3
"""Turns the output of MW2COOP_D3DCALLERS=1 (d3dcallers.txt of one or more runs) and tools/ghidra/FunctionOf.java's answer into a table of the exe functions that call Direct3D 9, by method.

  python -I tools/d3dcallers_report.py <funcs.txt from FunctionOf> <d3dcallers.txt> [<d3dcallers.txt> ...]
"""
import sys
from collections import defaultdict

DEVICE = ["QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
          "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus",
          "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
          "CreateDepthStencilSurface", "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill", "CreateOffscreenPlainSurface", "SetRenderTarget",
          "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport",
          "SetMaterial", "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock",
          "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
          "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
          "GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices",
          "CreateVertexDeclaration", "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader", "GetVertexShader", "SetVertexShaderConstantF",
          "GetVertexShaderConstantF", "SetVertexShaderConstantI", "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource", "GetStreamSource",
          "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF", "GetPixelShaderConstantF",
          "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB", "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery"]
IUNK = ["QueryInterface", "AddRef", "Release"]
RESOURCE = IUNK + ["GetDevice", "SetPrivateData", "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType"]
BASETEX = RESOURCE + ["SetLOD", "GetLOD", "GetLevelCount", "SetAutoGenFilterType", "GetAutoGenFilterType", "GenerateMipSubLevels"]
NAMES = {
    "Device": DEVICE,
    "D3D9": IUNK + ["RegisterSoftwareDevice", "GetAdapterCount", "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType", "CheckDeviceFormat",
                    "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "CheckDeviceFormatConversion", "GetDeviceCaps", "GetAdapterMonitor", "CreateDevice"],
    "Texture": BASETEX + ["GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect"],
    "Cube": BASETEX + ["GetLevelDesc", "GetCubeMapSurface", "LockRect", "UnlockRect", "AddDirtyRect"],
    "Volume": BASETEX + ["GetLevelDesc", "GetVolumeLevel", "LockBox", "UnlockBox", "AddDirtyBox"],
    "Surface": RESOURCE + ["GetContainer", "GetDesc", "LockRect", "UnlockRect", "GetDC", "ReleaseDC"],
    "VertexBuffer": RESOURCE + ["Lock", "Unlock", "GetDesc"],
    "IndexBuffer": RESOURCE + ["Lock", "Unlock", "GetDesc"],
    "Query": IUNK + ["GetDevice", "GetType", "GetDataSize", "Issue", "GetData"],
    "VertexShader": IUNK + ["GetDevice", "GetFunction"],
    "PixelShader": IUNK + ["GetDevice", "GetFunction"],
    "Declaration": IUNK + ["GetDevice", "GetDeclaration"],
    "SwapChain": IUNK + ["Present", "GetFrontBufferData", "GetBackBuffer", "GetRasterStatus", "GetDisplayMode", "GetDevice", "GetPresentParameters"],
}


def main():
    funcs = {}
    for line in open(sys.argv[1]):
        p = line.split()
        if len(p) >= 4:
            funcs[p[0]] = (p[1], p[2], p[3])
    per_func = defaultdict(lambda: defaultdict(int))
    mod_sites = defaultdict(lambda: defaultdict(int))
    seen = set()
    for path in sys.argv[2:]:
        for line in open(path):
            cls, slot, mod, rva, count = line.split()
            key = (cls, slot, mod, rva)
            slot = int(slot)
            name = NAMES.get(cls, [])
            meth = "%s.%s" % (cls, name[slot] if slot < len(name) else "slot%d" % slot)
            if key in seen:
                continue  # the same call site in another run
            seen.add(key)
            if mod.lower().startswith("iw4sp"):
                f = funcs.get(rva, ("?", "?", "0"))
                per_func[f[0]][meth] += int(count)
            else:
                mod_sites[mod][meth] += 1
    rows = sorted(per_func.items(), key=lambda kv: int(kv[0], 16) if kv[0] != "?" else 1 << 60)
    print("| function (rva) | Direct3D 9 methods called (calls counted in the runs) |")
    print("|---|---|")
    for rva, ms in rows:
        print("| 0x%s | %s |" % (rva, ", ".join("%s x%d" % (m, n) for m, n in sorted(ms.items(), key=lambda kv: -kv[1]))))
    print()
    print("%d functions of the exe call Direct3D 9" % len(rows))
    methods = defaultdict(set)
    for rva, ms in per_func.items():
        for m in ms:
            methods[m].add(rva)
    print("\n| method | functions |\n|---|---|")
    for m, fs in sorted(methods.items(), key=lambda kv: -len(kv[1])):
        print("| %s | %d |" % (m, len(fs)))
    print("\nother modules (call sites by method)")
    for mod, ms in mod_sites.items():
        print("- %s: %d sites: %s" % (mod, sum(ms.values()), ", ".join("%s %d" % (m, n) for m, n in sorted(ms.items(), key=lambda kv: -kv[1]))))


main()
