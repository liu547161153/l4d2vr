import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class FindClientClassSymbols extends GhidraScript {

	private void printMatches(String needle, int limit) {
		String needleLower = needle.toLowerCase();
		println("=== SYMBOL MATCHES: " + needle + " ===");

		SymbolTable symbolTable = currentProgram.getSymbolTable();
		List<Symbol> matches = new ArrayList<>();
		Iterator<Symbol> iterator = symbolTable.getAllSymbols(true);
		while (iterator.hasNext()) {
			Symbol symbol = iterator.next();
			String name = symbol.getName(true);
			if (name != null && name.toLowerCase().contains(needleLower)) {
				matches.add(symbol);
			}
		}

		Collections.sort(matches, Comparator.comparingLong(s -> s.getAddress().getOffset()));
		if (matches.isEmpty()) {
			println("  <none>");
			return;
		}

		int count = 0;
		for (Symbol symbol : matches) {
			printf("  %s  %-12s  %s\n",
				symbol.getAddress(),
				String.valueOf(symbol.getSymbolType()),
				symbol.getName(true));
			if (++count >= limit) {
				break;
			}
		}
	}

	@Override
	protected void run() throws Exception {
		println("PROGRAM: " + currentProgram.getExecutablePath());
		println("LANGUAGE: " + currentProgram.getLanguageID());
		println("IMAGE BASE: " + currentProgram.getImageBase());

		String[] needles = new String[] {
			"C_BaseEntity",
			"C_BaseAnimating",
			"C_BaseFlex",
			"C_BaseCombatCharacter",
			"CTerrorPlayer",
			"C_TerrorPlayer",
			"ClientRagdoll",
			"SetupBones",
			"UpdateClientSideAnimation",
			"ShouldInterpolate",
			"vftable"
		};

		for (String needle : needles) {
			printMatches(needle, 40);
		}
	}
}
