from ghidra.program.model.symbol import SymbolType


def print_matches(needle, limit=40):
    needle_lower = needle.lower()
    print("=== SYMBOL MATCHES: %s ===" % needle)

    symbol_table = currentProgram.getSymbolTable()
    matches = []
    it = symbol_table.getAllSymbols(True)
    while it.hasNext():
        sym = it.next()
        name = sym.getName(True)
        if needle_lower in name.lower():
            matches.append(sym)

    matches.sort(key=lambda s: s.getAddress().getOffset())
    if not matches:
        print("  <none>")
        return

    for sym in matches[:limit]:
        print(
            "  %s  %-12s  %s"
            % (sym.getAddress(), str(sym.getSymbolType()), sym.getName(True))
        )


def main():
    print("PROGRAM: %s" % currentProgram.getExecutablePath())
    print("LANGUAGE: %s" % currentProgram.getLanguageID())
    print("IMAGE BASE: %s" % currentProgram.getImageBase())

    needles = [
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
        "vftable",
    ]

    for needle in needles:
        print_matches(needle)


main()
