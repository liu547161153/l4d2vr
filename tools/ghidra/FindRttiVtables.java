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
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class FindRttiVtables extends GhidraScript {

	private List<Symbol> findMatches(String needle) {
		List<Symbol> matches = new ArrayList<>();
		String loweredNeedle = needle.toLowerCase();
		SymbolTable symbolTable = currentProgram.getSymbolTable();
		Iterator<Symbol> iterator = symbolTable.getAllSymbols(true);
		while (iterator.hasNext()) {
			Symbol symbol = iterator.next();
			String name = symbol.getName(true);
			if (name != null && name.toLowerCase().contains(loweredNeedle)) {
				matches.add(symbol);
			}
		}
		Collections.sort(matches, Comparator.comparingLong(s -> s.getAddress().getOffset()));
		return matches;
	}

	private List<Address> findPointersTo(Address target) throws Exception {
		List<Address> matches = new ArrayList<>();
		Memory memory = currentProgram.getMemory();
		int targetValue = (int) target.getUnsignedOffset();

		for (MemoryBlock block : memory.getBlocks()) {
			if (!block.isInitialized() || !block.isRead()) {
				continue;
			}

			Address start = block.getStart();
			Address end = block.getEnd();
			long startOffset = start.getUnsignedOffset();
			long endOffset = end.getUnsignedOffset();

			for (long offset = startOffset; offset + 3 <= endOffset; offset += 4) {
				Address candidate = toAddr(offset);
				if (memory.getInt(candidate) == targetValue) {
					matches.add(candidate);
				}
			}
		}

		return matches;
	}

	private String describe(Address address) {
		FunctionManager functionManager = currentProgram.getFunctionManager();
		Function function = functionManager.getFunctionContaining(address);
		if (function != null) {
			return function.getName(true);
		}

		Symbol primary = getSymbolAt(address);
		if (primary != null) {
			return primary.getName(true);
		}

		Symbol[] symbols = currentProgram.getSymbolTable().getSymbols(address);
		if (symbols != null && symbols.length > 0) {
			return symbols[0].getName(true);
		}

		return "<no symbol>";
	}

	private void printVtableSlice(Address vtable, int count) throws Exception {
		Memory memory = currentProgram.getMemory();
		printf("  VTABLE @ %s\n", vtable);
		for (int slot = 0; slot < count; ++slot) {
			Address entry = vtable.add(slot * 4L);
			if (!memory.contains(entry)) {
				printf("    [%d] %s  <out of range>\n", slot, entry);
				continue;
			}

			long target = Integer.toUnsignedLong(memory.getInt(entry));
			Address targetAddr = toAddr(target);
			printf("    [%d] %s -> %s  %s\n", slot, entry, targetAddr, describe(targetAddr));
		}
	}

	@Override
	protected void run() throws Exception {
		if (getScriptArgs().length < 1) {
			println("Usage: FindRttiVtables <needle> [needle2 ...] [--slots=N]");
			return;
		}

		int slots = 12;
		List<String> needles = new ArrayList<>();
		for (String arg : getScriptArgs()) {
			if (arg.startsWith("--slots=")) {
				slots = Integer.parseInt(arg.substring("--slots=".length()));
				continue;
			}
			needles.add(arg);
		}

		for (String needle : needles) {
			List<Symbol> matches = findMatches(needle);
			if (matches.isEmpty()) {
				printf("No symbols matched: %s\n", needle);
				continue;
			}

			for (Symbol symbol : matches) {
				Address typeDesc = symbol.getAddress();
				printf("=== RTTI TYPE: %s @ %s ===\n", symbol.getName(true), typeDesc);

				List<Address> typeRefs = findPointersTo(typeDesc);
				if (typeRefs.isEmpty()) {
					println("  <no complete object locator refs>");
					continue;
				}

				for (Address typeRef : typeRefs) {
					Address col = typeRef.subtract(0xc);
					Memory memory = currentProgram.getMemory();
					if (!memory.contains(col) || !memory.contains(col.add(0x10))) {
						continue;
					}

					int signature = memory.getInt(col);
					int offset = memory.getInt(col.add(4));
					int cdOffset = memory.getInt(col.add(8));
					int typePtr = memory.getInt(col.add(0xc));
					if (Integer.toUnsignedLong(typePtr) != typeDesc.getUnsignedOffset()) {
						continue;
					}

					printf("  COL @ %s  signature=%d offset=%d cdOffset=%d\n", col, signature, offset, cdOffset);

					List<Address> colRefs = findPointersTo(col);
					if (colRefs.isEmpty()) {
						println("    <no vtable refs>");
						continue;
					}

					for (Address colRef : colRefs) {
						Address vtable = colRef.add(4);
						printf("    COL ref @ %s\n", colRef);
						printVtableSlice(vtable, slots);
					}
				}
			}
		}
	}
}
