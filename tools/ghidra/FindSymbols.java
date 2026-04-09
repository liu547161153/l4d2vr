import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class FindSymbols extends GhidraScript {

	@Override
	protected void run() throws Exception {
		if (getScriptArgs().length < 1) {
			println("Usage: FindSymbols <needle> [needle2 ...] [--limit=N]");
			return;
		}

		int limit = 80;
		List<String> needles = new ArrayList<>();
		for (String arg : getScriptArgs()) {
			if (arg.startsWith("--limit=")) {
				limit = Integer.parseInt(arg.substring("--limit=".length()));
				continue;
			}
			needles.add(arg);
		}

		if (needles.isEmpty()) {
			println("No search needles provided.");
			return;
		}

		println("PROGRAM: " + currentProgram.getExecutablePath());
		SymbolTable symbolTable = currentProgram.getSymbolTable();
		for (String needle : needles) {
			final String loweredNeedle = needle.toLowerCase();
			println("NEEDLE: " + needle);

			List<Symbol> matches = new ArrayList<>();
			Iterator<Symbol> iterator = symbolTable.getAllSymbols(true);
			while (iterator.hasNext()) {
				Symbol symbol = iterator.next();
				String name = symbol.getName(true);
				if (name != null && name.toLowerCase().contains(loweredNeedle))
					matches.add(symbol);
			}

			Collections.sort(matches, Comparator.comparingLong(s -> s.getAddress().getOffset()));
			if (matches.isEmpty()) {
				println("<none>");
				continue;
			}

			for (int i = 0; i < matches.size() && i < limit; ++i) {
				Symbol symbol = matches.get(i);
				printf("%s  %-12s  %s\n",
					symbol.getAddress(),
					String.valueOf(symbol.getSymbolType()),
					symbol.getName(true));
			}
		}
	}
}
