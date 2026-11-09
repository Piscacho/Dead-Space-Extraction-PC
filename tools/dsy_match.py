"""Name SDK functions by matching them against Dolphin's signature database (totaldb.dsy).

The hash is Dolphin's HashSignatureDB::ComputeCodeChecksum: it keeps each instruction's
opcode and register fields but drops immediates, displacements and branch targets, so the
same SDK function hashes the same in every game. A match needs equal hash and size.

Usage: python tools/dsy_match.py [path/to/totaldb.dsy]
Writes config/sdk_names.txt ("0xADDRESS name size", one per line).
"""
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DSY = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("D:/Emulador Wii/dolphin-2503a-x64/Dolphin-x64/Sys/totaldb.dsy")
DOL = ROOT.parent / "juego" / "DATA" / "sys" / "main.dol"
SYMBOLS = ROOT.parent / "decomp" / "config" / "RZJE69" / "symbols.txt"
OUT = ROOT / "config" / "sdk_names.txt"


def load_db():
    data = DSY.read_bytes()
    count, = struct.unpack_from("<I", data, 0)
    db = defaultdict(list)
    for i in range(count):
        checksum, size = struct.unpack_from("<II", data, 4 + i * 136)
        name = data[12 + i * 136: 12 + i * 136 + 128].split(b"\0")[0].decode("latin1")
        db[checksum].append((size, name))
    return count, db


def read_dol():
    dol = DOL.read_bytes()
    u32 = lambda o: struct.unpack_from(">I", dol, o)[0]
    secs = [(u32(0x48 + 4 * i), u32(4 * i), u32(0x90 + 4 * i)) for i in range(18) if u32(0x90 + 4 * i)]

    def word(addr):
        for a, off, size in secs:
            if a <= addr < a + size:
                return u32(off + addr - a)
        return 0
    return word


def checksum(word, start, size):
    s = 0
    for addr in range(start, start + size, 4):
        opcode = word(addr)
        op = opcode & 0xFC000000
        op2 = op3 = 0
        aux = op >> 26
        if aux == 4:  # paired singles
            op2 = opcode & 0x0000003F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x000007C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):  # addi, mulli, cmpi...
            op2 = opcode & 0x03FF0000
        elif aux in (19, 31, 63):  # cr, integer, fpu
            op2 = opcode & 0x000007FF
        elif aux == 59:  # fpu single
            op2 = opcode & 0x0000003F
            if op2 < 16:
                op3 = opcode & 0x000007C0
        elif 32 <= aux < 48:  # loads and stores
            op2 = opcode & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)
        s ^= op | op2 | op3
    return s


def main():
    count, db = load_db()
    word = read_dol()
    pat = re.compile(r"^(\S+) = \.(?:init|text):0x([0-9A-F]{8}); // type:function size:0x([0-9A-F]+)")
    funcs = []
    for line in SYMBOLS.read_text().splitlines():
        m = pat.match(line)
        if m and not m.group(1).startswith("gap_"):
            funcs.append((int(m.group(2), 16), int(m.group(3), 16)))
    named, ambiguous = [], 0
    for addr, size in funcs:
        # Two-instruction functions (getters, stubs) hash alike across unrelated names.
        if size <= 8:
            continue
        cands = sorted({n for s, n in db.get(checksum(word, addr, size), []) if s == size})
        if len(cands) == 1:
            named.append((addr, cands[0], size))
        elif cands:
            ambiguous += 1
            named.append((addr, cands[0] + "?", size))  # "?" marks several names with the same code
    OUT.write_bytes("".join(f"0x{a:08X} {n} 0x{s:X}\n" for a, n, s in named).encode())
    print(f"database: {count} signatures; game functions: {len(funcs)}")
    print(f"named: {len(named)} ({ambiguous} with several candidates) -> {OUT}")


if __name__ == "__main__":
    main()
