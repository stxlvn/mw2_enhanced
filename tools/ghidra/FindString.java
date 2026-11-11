// Defined strings that contain a text, with the functions that reference them (to find the code that prints an error message).
// Usage (headless): ... -postScript FindString.java <out file> <text> [<text> ...]
// @category mw2coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;

public class FindString extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		long base = currentProgram.getImageBase().getOffset();
		FileWriter out = new FileWriter(args[0]);
		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext()) {
			Data d = it.next();
			Object v = d.getValue();
			if (!(v instanceof String))
				continue;
			String s = (String) v;
			for (int i = 1; i < args.length; i++) {
				if (!s.contains(args[i]))
					continue;
				out.write(String.format("rva 0x%x: \"%s\"\n", d.getAddress().getOffset() - base, s.replace("\n", "\\n")));
				for (Reference r : getReferencesTo(d.getAddress())) {
					Function f = getFunctionContaining(r.getFromAddress());
					out.write(String.format("    referenced from rva 0x%x in function %s\n", r.getFromAddress().getOffset() - base, f == null ? "?" : "rva 0x" + Long.toHexString(f.getEntryPoint().getOffset() - base)));
				}
				break;
			}
		}
		out.close();
	}
}
