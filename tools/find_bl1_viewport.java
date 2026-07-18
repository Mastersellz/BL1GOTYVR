// Find UGameViewportClient RTTI, vtables, and related functions in BL1 GOTY Enhanced.
// @category VRMod

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class find_bl1_viewport extends GhidraScript {
    private boolean matches(String value) {
        return value != null && value.toLowerCase().contains("gameviewportclient");
    }

    private void printReferences(Address target, int depth, String indent) {
        if (depth > 3) return;
        ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(target);
        while (references.hasNext()) {
            Reference reference = references.next();
            Address from = reference.getFromAddress();
            Function function = currentProgram.getFunctionManager().getFunctionContaining(from);
            long rva = from.subtract(currentProgram.getImageBase());
            println(String.format("%sref RVA 0x%08X type=%s function=%s", indent, rva,
                reference.getReferenceType(), function == null ? "<data>" : function.getName()));
            if (function == null) printReferences(from, depth + 1, indent + "  ");
        }
    }

    @Override
    public void run() throws Exception {
        println("=== BL1 GOTY UGameViewportClient analysis ===");
        Listing listing = currentProgram.getListing();
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext()) {
            Symbol symbol = symbols.next();
            if (!matches(symbol.getName(true))) continue;
            Address address = symbol.getAddress();
            println(String.format("symbol %s at RVA 0x%08X type=%s", symbol.getName(true),
                address.subtract(currentProgram.getImageBase()), symbol.getSymbolType()));
            printReferences(address, 0, "  ");
        }

        for (Data data : listing.getDefinedData(true)) {
            if (!data.hasStringValue() || !matches(data.getValue().toString())) continue;
            Address address = data.getAddress();
            println(String.format("string %s at RVA 0x%08X", data.getValue(),
                address.subtract(currentProgram.getImageBase())));
            printReferences(address, 0, "  ");
        }

        FunctionManager functions = currentProgram.getFunctionManager();
        for (Function function : functions.getFunctions(true)) {
            if (matches(function.getName(true))) {
                println(String.format("function %s at RVA 0x%08X", function.getName(true),
                    function.getEntryPoint().subtract(currentProgram.getImageBase())));
            }
        }
    }
}
