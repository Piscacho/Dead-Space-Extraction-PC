"""Write config/dse_switch_tables.toml (XenonRecomp jump tables) from dtk's assembly.

CodeWarrior compiles a switch as:
    cmplwi rI, N ; bgt default
    lis rA, jumptable@ha ; slwi rB, rI, 2 ; addi rA, rA, jumptable@l
    lwzx rT, rA, rB ; mtctr rT ; bctr
For every bctr, this walks back to the lwzx, finds the index register (source of the
slwi) and the table symbol (from the addi), and reads the case targets from main.dol.
Without these tables XenonRecomp turns each switch into an indirect call.

Usage: python tools/make_switch_tables.py
"""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASM = ROOT.parent / "decomp" / "build" / "RZJE69" / "asm"
SYMBOLS = ROOT.parent / "decomp" / "config" / "RZJE69" / "symbols.txt"
DOL = ROOT.parent / "juego" / "DATA" / "sys" / "main.dol"
OUT = ROOT / "config" / "dse_switch_tables.toml"

INS = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  .. .. .. .. \*/\t(\S+)\s*(.*)$")


def dol_reader():
    dol = DOL.read_bytes()
    u32 = lambda o: struct.unpack_from(">I", dol, o)[0]
    secs = [(u32(0x48 + 4 * i), u32(4 * i), u32(0x90 + 4 * i)) for i in range(18) if u32(0x90 + 4 * i)]

    def word(addr):
        for a, off, size in secs:
            if a <= addr < a + size:
                return u32(off + addr - a)
        raise KeyError(hex(addr))
    return word


def main():
    word = dol_reader()
    # Tables are named jumptable_X by dtk, or keep a compiler label like "@4022_804A3DC0".
    tables = {}
    for line in SYMBOLS.read_text().splitlines():
        m = re.match(r'^"?([^"\s]+)"? = \.\w+:0x([0-9A-F]{8}); // type:object size:0x([0-9A-F]+)', line)
        if m:
            tables[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

    # One copy per address: the asm folder can hold stale files from older split layouts.
    instrs = {}
    for path in ASM.rglob("*.s"):
        for line in path.read_text().splitlines():
            m = INS.match(line)
            if m:
                instrs[int(m.group(1), 16)] = (m.group(2), m.group(3))

    entries, missed = [], []
    for addr in sorted(a for a, (op, _) in instrs.items() if op == "bctr"):
        lwzx = idx = table = default = bound = boundReg = None
        shift = 0
        scaleBack = scaleAmt = boundBack = None
        for back in range(1, 12):
            op, args = instrs.get(addr - 4 * back, ("", ""))
            regs = [a.strip() for a in args.split(",")]
            if op == "lwzx" and lwzx is None:
                lwzx = regs  # rT, rA, rB
            elif lwzx and op in ("slwi", "rlwinm") and regs[0] == lwzx[2] and idx is None:
                idx = int(regs[1][1:])
                scaleBack, scaleAmt = back, int(regs[2], 0)
                # Scaled in place (rlwinm r0,r0,2,...): at the bctr the register holds index << n.
                if regs[0] == regs[1]:
                    shift = int(regs[2], 0)
            elif lwzx and op == "addi" and regs[0] == lwzx[1] and "@l" in args and table is None:
                table = re.search(r'"?([^"\s,]+)"?@l', args).group(1)
            elif op == "bgt" and default is None and back > 3:
                t = re.search(r"\.L_([0-9A-F]{8})|fn_([0-9A-F]{8})", args)
                if t:
                    default = int(t.group(1) or t.group(2), 16)
            elif op == "cmplwi" and default is not None and bound is None:
                # Bounds check before the bgt: index register and last case.
                boundReg, bound = int(regs[0][1:]), int(regs[1], 0)
                boundBack = back
            if op in ("blr", "b") and back > 1:
                break
        # The index register can be overwritten between the scaling and the bctr (for example
        # rlwinm r9,r3,2 ; lwzx r3,r3,r9): then switch on the scaled register instead.
        if idx is not None and scaleBack is not None and shift == 0:
            for b in range(1, scaleBack):
                op2, args2 = instrs.get(addr - 4 * b, ("", ""))
                r2 = [a.strip() for a in args2.split(",")]
                if op2 and not op2.startswith(("st", "cmp", "mt", "b")) and r2 and r2[0] == f"r{idx}":
                    idx, shift = int(lwzx[2][1:]), scaleAmt
                    break
        # The scaled index may be computed outside the window: use the bounds-check register.
        if idx is None and boundReg is not None:
            idx = boundReg
            # Same problem as above: if the bounds-check register is overwritten before the
            # bctr, the scaled register (lwzx's index, index << 2) is the one left.
            for b in range(1, boundBack):
                op2, args2 = instrs.get(addr - 4 * b, ("", ""))
                r2 = [a.strip() for a in args2.split(",")]
                if op2 and not op2.startswith(("st", "cmp", "mt", "b")) and r2 and r2[0] == f"r{idx}":
                    idx, shift = int(lwzx[2][1:]), 2
                    break
        if table in tables:
            taddr, tsize = tables[table]
            count = tsize // 4
        elif table and re.search(r"_([0-9A-F]{8})$", table) and bound is not None:
            # Compiler label not in symbols.txt: address from its name, size from the bound.
            taddr, count = int(re.search(r"_([0-9A-F]{8})$", table).group(1), 16), bound + 1
        else:
            taddr = None
        if idx is None or taddr is None:
            missed.append(addr)
            continue
        labels = [word(taddr + 4 * i) for i in range(count)]
        entries.append((addr, idx, default, labels, shift))

    out = ["# Generated by tools/make_switch_tables.py from dtk's assembly and jump tables."]
    for addr, idx, default, labels, shift in entries:
        out.append("[[switch]]")
        out.append(f"base = 0x{addr:08X}")
        out.append(f"r = {idx}")
        if shift:
            out.append(f"shift = {shift}")
        if default:
            out.append(f"default = 0x{default:08X}")
        out.append("labels = [")
        out += [f"    0x{l:08X}," for l in labels]
        out.append("]")
    OUT.write_bytes(("\n".join(out) + "\n").encode())
    print(f"bctr: {len(entries) + len(missed)}, tables written: {len(entries)}, not resolved: {len(missed)}")
    for a in missed[:20]:
        print(f"  unresolved bctr at 0x{a:08X}")


if __name__ == "__main__":
    main()
