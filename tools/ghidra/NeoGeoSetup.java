// Ghidra headless post-script: lays out the Neo Geo 68000 memory map around an
// imported P1 image (loaded at 0x000000) and seeds entry points.
// Args: <bios.be path> <p2.be path>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
import java.nio.file.*;

public class NeoGeoSetup extends GhidraScript {
    Address a(long x) { return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(x); }

    void fileBlock(String name, long base, byte[] data, boolean exec) throws Exception {
        Memory m = currentProgram.getMemory();
        MemoryBlock b = m.createInitializedBlock(name, a(base), new ByteArrayInputStream(data), data.length, monitor, false);
        b.setRead(true); b.setWrite(false); b.setExecute(exec);
    }
    void ramBlock(String name, long base, long len, boolean vol) throws Exception {
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, a(base), len, false);
        b.setRead(true); b.setWrite(true); b.setExecute(false); b.setVolatile(vol);
    }
    void entry(long addr, String name) throws Exception {
        Address ad = a(addr);
        SymbolTable st = currentProgram.getSymbolTable();
        st.createLabel(ad, name, SourceType.USER_DEFINED);
        st.addExternalEntryPoint(ad);
        new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
        new CreateFunctionCmd(ad).applyTo(currentProgram, monitor);
    }
    long be32(byte[] d, int o) { return ((d[o]&0xffL)<<24)|((d[o+1]&0xffL)<<16)|((d[o+2]&0xffL)<<8)|(d[o+3]&0xffL); }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        byte[] bios = Files.readAllBytes(Paths.get(args[0]));
        byte[] p2 = Files.readAllBytes(Paths.get(args[1]));
        MemoryBlock p1 = currentProgram.getMemory().getBlock(a(0));
        p1.setName("P1"); p1.setWrite(false); p1.setExecute(true);

        ramBlock("WORKRAM", 0x100000, 0x10000, false);
        byte[] bank0 = java.util.Arrays.copyOfRange(p2, 0, 0x100000);
        fileBlock("P2_BANK0", 0x200000, bank0, false);
        ramBlock("IO_3xxxxx", 0x300000, 0x100000, true);
        ramBlock("PALETTE", 0x400000, 0x2000, true);
        ramBlock("MEMCARD", 0x800000, 0x1000, true);
        fileBlock("BIOS", 0xC00000, bios, true);
        ramBlock("BACKUPRAM", 0xD00000, 0x10000, false);

        // Game header entry points (jmp vectors at 0x122..0x134)
        entry(0x122, "USER");
        entry(0x128, "PLAYER_START");
        entry(0x12E, "DEMO_END");
        entry(0x134, "COIN_SOUND");
        // Game exception vectors (game vector table in P1 at 0..0xFF)
        byte[] p1b = new byte[0x100];
        currentProgram.getMemory().getBytes(a(0), p1b);
        java.util.HashSet<Long> seen = new java.util.HashSet<>();
        for (int i = 2; i < 64; i++) {
            long v = be32(p1b, i*4);
            if (v < 0x100000 && v >= 0x100 && (v & 1) == 0 && seen.add(v)) entry(v, String.format("VEC%02d_%X", i, v));
        }
        // BIOS: its own vector table and the documented jump table at 0xC00402..0xC0058x
        for (int i = 1; i < 64; i++) {
            long v = be32(bios, i*4);
            if (v >= 0xC00000 && v < 0xC20000 && (v & 1) == 0 && seen.add(v)) entry(v, String.format("BIOSVEC%02d_%X", i, v));
        }
        for (long ad = 0xC00402; ad < 0xC00600; ad += 6) {
            int off = (int)(ad - 0xC00000);
            int op = ((bios[off]&0xff)<<8)|(bios[off+1]&0xff);
            if (op != 0x4EF9) break;
            if (seen.add(ad)) entry(ad, String.format("BIOS_JT_%X", ad));
        }
        println("NeoGeoSetup done");
    }
}
