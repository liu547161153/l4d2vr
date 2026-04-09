import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class DecompileAt extends GhidraScript {

	@Override
	protected void run() throws Exception {
		if (getScriptArgs().length < 1) {
			println("Usage: DecompileAt <address> [address2 ...] [--maxLines=N]");
			return;
		}

		int maxLines = 80;
		java.util.List<String> addresses = new java.util.ArrayList<>();
		for (int i = 0; i < getScriptArgs().length; ++i) {
			String arg = getScriptArgs()[i];
			if (arg.startsWith("--maxLines=")) {
				maxLines = Integer.parseInt(arg.substring("--maxLines=".length()));
				continue;
			}
			if ("--maxLines".equals(arg) && i + 1 < getScriptArgs().length) {
				maxLines = Integer.parseInt(getScriptArgs()[++i]);
				continue;
			}
			addresses.add(arg);
		}

		if (addresses.isEmpty()) {
			println("No addresses provided.");
			return;
		}

		DecompInterface ifc = new DecompInterface();
		DecompileOptions options = new DecompileOptions();
		ifc.setOptions(options);
		ifc.openProgram(currentProgram);

		FunctionManager fm = currentProgram.getFunctionManager();
		for (String addrText : addresses) {
			Address address = toAddr(addrText);
			if (address == null) {
				println("Invalid address: " + addrText);
				continue;
			}
			Function fn = fm.getFunctionContaining(address);
			if (fn == null) {
				println("No function contains address: " + address);
				continue;
			}

			println("FUNCTION: " + fn.getName(true));
			println("ENTRY: " + fn.getEntryPoint());
			println("SIGNATURE: " + fn.getSignature().getPrototypeString(true));

			DecompileResults results = ifc.decompileFunction(fn, 60, monitor);
			if (!results.decompileCompleted()) {
				println("DECOMPILE FAILED: " + results.getErrorMessage());
				continue;
			}

			String c = results.getDecompiledFunction().getC();
			String[] lines = c.split("\\R");
			for (int i = 0; i < lines.length && i < maxLines; ++i) {
				printf("%4d: %s\n", i + 1, lines[i]);
			}
		}

		ifc.dispose();
	}
}
