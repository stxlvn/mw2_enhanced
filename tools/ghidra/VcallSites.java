// For each given vtable displacement, the number of CALL [reg + disp] sites in the whole program and the functions they are in (feasibility of a native Direct3D backend:
// how many places make the calls a backend has to take over: draws, state, shaders, textures, buffers).
// Usage (headless): ... -postScript VcallSites.java <out file> <disp hex> [<disp hex> ...]
// @category mw2coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.FileWriter;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.TreeSet;

public class VcallSites extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		long base = currentProgram.getImageBase().getOffset();
		Map<Long, Integer> sites = new HashMap<>();
		Map<Long, LinkedHashSet<Long>> funcs = new HashMap<>();
		for (int i = 1; i < args.length; i++) {
			long d = Long.parseLong(args[i].replace("0x", ""), 16);
			sites.put(d, 0);
			funcs.put(d, new LinkedHashSet<>());
		}
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext()) {
			Instruction ins = it.next();
			if (!ins.getMnemonicString().equals("CALL"))
				continue;
			String s = ins.toString();
			int lb = s.indexOf("[");
			int p = s.indexOf("+ 0x", lb < 0 ? 0 : lb);
			if (lb < 0 || p < 0)
				continue;
			int e = s.indexOf("]", p);
			if (e < 0)
				continue;
			long disp;
			try {
				disp = Long.parseLong(s.substring(p + 4, e).trim(), 16);
			} catch (Exception ex) {
				continue;
			}
			if (!sites.containsKey(disp))
				continue;
			sites.merge(disp, 1, Integer::sum);
			Function f = getFunctionContaining(ins.getAddress());
			funcs.get(disp).add(f == null ? 0L : f.getEntryPoint().getOffset() - base);
		}
		FileWriter out = new FileWriter(args[0]);
		for (int i = 1; i < args.length; i++) {
			long d = Long.parseLong(args[i].replace("0x", ""), 16);
			out.write(String.format("+0x%03x: %d sites in %d functions: ", d, sites.get(d), funcs.get(d).size()));
			int n = 0;
			for (long f : new TreeSet<>(funcs.get(d))) {
				if (n++ >= 12)
					break;
				out.write(String.format("0x%x ", f));
			}
			out.write("\n");
		}
		out.close();
	}
}
