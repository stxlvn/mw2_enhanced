// Survey of the renderer's Direct3D 9 use (feasibility of a native Direct3D 9Ex / 11 / 12 backend): the functions that read the device global, and the calls through the
// device's vtable made in them (CALL [reg + disp], disp a multiple of 8 below 0x800), as a histogram by IDirect3DDevice9 method, plus the biggest functions by such calls.
// Usage (headless): analyzeHeadless <project> <name> -process <exe> -noanalysis -readOnly -scriptPath tools/ghidra -postScript RendererSurvey.java <out file> <device global rva hex>
// @category mw2coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class RendererSurvey extends GhidraScript {
	static final String[] NAMES = {"QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources", "GetDirect3D", "GetDeviceCaps",
		"GetDisplayMode", "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains",
		"Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
		"CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface", "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData",
		"StretchRect", "ColorFill", "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene", "EndScene",
		"Clear", "SetTransform", "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight", "LightEnable",
		"GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock", "BeginStateBlock", "EndStateBlock", "SetClipStatus",
		"GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState", "ValidateDevice", "SetPaletteEntries",
		"GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing",
		"GetSoftwareVertexProcessing", "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices",
		"CreateVertexDeclaration", "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader", "GetVertexShader",
		"SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetVertexShaderConstantI", "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB",
		"SetStreamSource", "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader",
		"SetPixelShaderConstantF", "GetPixelShaderConstantF", "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB",
		"DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery"};

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		long base = currentProgram.getImageBase().getOffset();
		Address dev = currentProgram.getAddressFactory().getAddress(Long.toHexString(base + Long.parseLong(args[1].replace("0x", ""), 16)));
		HashSet<Address> funcs = new HashSet<>();
		int refs = 0;
		for (Reference r : getReferencesTo(dev)) {
			refs++;
			Function f = getFunctionContaining(r.getFromAddress());
			if (f != null)
				funcs.add(f.getEntryPoint());
		}
		TreeMap<Long, Integer> hist = new TreeMap<>();
		Map<Address, Integer> perFunc = new HashMap<>();
		Map<Address, Integer> funcSize = new HashMap<>();
		for (Address fe : funcs) {
			Function f = getFunctionAt(fe);
			if (f == null)
				continue;
			int n = 0;
			InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
			while (it.hasNext()) {
				Instruction ins = it.next();
				if (!ins.getMnemonicString().equals("CALL"))
					continue;
				String s = ins.toString();
				int lb = s.indexOf("[");
				if (lb < 0 || !s.contains("+ 0x"))
					continue;
				int p = s.indexOf("+ 0x", lb);
				int e = s.indexOf("]", p);
				if (e < 0)
					continue;
				long disp;
				try {
					disp = Long.parseLong(s.substring(p + 4, e).trim(), 16);
				} catch (Exception ex) {
					continue;
				}
				if (disp % 8 != 0 || disp >= 0x800)
					continue;
				hist.merge(disp, 1, Integer::sum);
				n++;
			}
			perFunc.put(fe, n);
			funcSize.put(fe, (int) f.getBody().getNumAddresses());
		}
		FileWriter out = new FileWriter(args[0]);
		out.write("device global " + dev + ": " + refs + " references in " + funcs.size() + " functions\n\n");
		out.write("calls through [reg+disp] (disp multiple of 8 below 0x800) in those functions, by IDirect3DDevice9 slot (a count includes non-device virtual calls made in the same functions):\n");
		for (Map.Entry<Long, Integer> e : hist.entrySet()) {
			int idx = (int) (e.getKey() / 8);
			out.write(String.format("  +0x%03x slot %3d %-30s %5d\n", e.getKey(), idx, idx < NAMES.length ? NAMES[idx] : "?", e.getValue()));
		}
		List<Map.Entry<Address, Integer>> top = new ArrayList<>(perFunc.entrySet());
		top.sort((a, b) -> b.getValue() - a.getValue());
		out.write("\nfunctions with the most such calls (rva, calls, size in bytes):\n");
		for (int i = 0; i < 40 && i < top.size(); i++)
			out.write(String.format("  rva 0x%x  %d calls  %d bytes\n", top.get(i).getKey().getOffset() - base, top.get(i).getValue(), funcSize.get(top.get(i).getKey())));
		out.close();
	}
}
