import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class DumpXrefsToSymbol extends GhidraScript {

	private List<Symbol> findMatches(String needle) {
		List<Symbol> matches = new ArrayList<>();
		String needleLower = needle.toLowerCase();
		SymbolTable symbolTable = currentProgram.getSymbolTable();
		Iterator<Symbol> iterator = symbolTable.getAllSymbols(true);
		while (iterator.hasNext()) {
			Symbol symbol = iterator.next();
			String name = symbol.getName(true);
			if (name != null && name.toLowerCase().contains(needleLower)) {
				matches.add(symbol);
			}
		}
		Collections.sort(matches, Comparator.comparingLong(s -> s.getAddress().getOffset()));
		return matches;
	}

	@Override
	protected void run() throws Exception {
		FunctionManager functionManager = currentProgram.getFunctionManager();
		String[] needles = getScriptArgs().length > 0
			? getScriptArgs()
			: new String[] { "s_C_BaseAnimating::SetupBones" };

		for (String needle : needles) {
			List<Symbol> matches = findMatches(needle);
			if (matches.isEmpty()) {
				println("No symbols matched: " + needle);
				continue;
			}

			for (Symbol symbol : matches) {
				Address target = symbol.getAddress();
				println("=== XREFS TO " + symbol.getName(true) + " @ " + target + " ===");
				Reference[] refs = getReferencesTo(target);
				int count = 0;
				for (Reference ref : refs) {
					Address from = ref.getFromAddress();
					Function fn = functionManager.getFunctionContaining(from);
					String fnName = fn != null ? fn.getName(true) : "<no function>";
					printf("  %s  %-12s  %s\n", from, ref.getReferenceType().toString(), fnName);
					count++;
				}
				if (count == 0) {
					println("  <no xrefs>");
				}
			}
		}
	}
}
