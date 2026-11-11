// For a file of hex rvas (one per line), the function that contains each: "<rva> <entry rva> <name> <size in bytes>" per line into the output file.
// Usage (headless): ... -postScript FunctionOf.java <out file> <in file>
// @category mw2coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.FileWriter;

public class FunctionOf extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] a = getScriptArgs();
		long base = currentProgram.getImageBase().getOffset();
		FileWriter out = new FileWriter(a[0]);
		BufferedReader in = new BufferedReader(new FileReader(a[1]));
		String line;
		while ((line = in.readLine()) != null) {
			line = line.trim();
			if (line.isEmpty())
				continue;
			long rva = Long.parseLong(line, 16);
			Address at = currentProgram.getAddressFactory().getAddress(Long.toHexString(base + rva));
			Function f = at == null ? null : getFunctionContaining(at);
			if (f == null)
				out.write(line + " ? ? 0\n");
			else
				out.write(line + " " + Long.toHexString(f.getEntryPoint().getOffset() - base) + " " + f.getName() + " " + f.getBody().getNumAddresses() + "\n");
		}
		out.close();
	}
}
