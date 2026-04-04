import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class DumpVtableSlice extends GhidraScript {

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

	private String describeTarget(Address target) {
		FunctionManager functionManager = currentProgram.getFunctionManager();
		Function function = functionManager.getFunctionContaining(target);
		if (function != null) {
			return function.getName(true);
		}

		Symbol primary = getSymbolAt(target);
		if (primary != null) {
			return primary.getName(true);
		}

		Symbol[] symbols = currentProgram.getSymbolTable().getSymbols(target);
		if (symbols != null && symbols.length > 0) {
			return symbols[0].getName(true);
		}

		return "<no symbol>";
	}

	@Override
	protected void run() throws Exception {
		String needle = getScriptArgs().length > 0 ? getScriptArgs()[0] : "C_BaseEntity::vftable";
		int startIndex = getScriptArgs().length > 1 ? Integer.parseInt(getScriptArgs()[1]) : 0;
		int count = getScriptArgs().length > 2 ? Integer.parseInt(getScriptArgs()[2]) : 32;
		boolean lastOnly = getScriptArgs().length > 3 && "last".equalsIgnoreCase(getScriptArgs()[3]);

		List<Symbol> matches = findMatches(needle);
		if (matches.isEmpty()) {
			println("No vtable symbols matched: " + needle);
			return;
		}
		if (lastOnly) {
			Symbol selected = matches.get(matches.size() - 1);
			matches = new ArrayList<>();
			matches.add(selected);
		}

		Memory memory = currentProgram.getMemory();
		for (Symbol symbol : matches) {
			println("=== VTABLE: " + symbol.getName(true) + " @ " + symbol.getAddress() + " ===");
			for (int i = 0; i < count; i++) {
				int slot = startIndex + i;
				Address entryAddress = symbol.getAddress().add(slot * 4L);
				if (!memory.contains(entryAddress)) {
					printf("  [%d] %s  <out of memory>\n", slot, entryAddress);
					continue;
				}

				int raw = memory.getInt(entryAddress);
				long unsigned = Integer.toUnsignedLong(raw);
				Address target = toAddr(unsigned);
				printf("  [%d] %s -> %s  %s\n", slot, entryAddress, target, describeTarget(target));
			}
		}
	}
}
