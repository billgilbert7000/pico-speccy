#!/usr/bin/env python3
"""Reconstruct the Scorpion GMX ROM image from a LINKED ELF and diff it against the dump.

rom_pack.py verifies its own mapping in memory and rom_verify.py verifies the GENERATED
arrays; neither can catch a linker- or binding-level mistake (a base that resolved to a
private copy, a symbol dropped by --gc-sections, an overlay bound to the wrong bank).
This does: it takes the 32 {data, overlay} rows out of scorpion_gmx_banks.h, resolves
every symbol against the firmware's own bytes, applies the overlays and compares the
512 KB with src/roms/scorpion/src/<image>.bin.

    python3 tools/gmx_elf_check.py build-ZERO2-PIOUSB/bin/MinSizeRel/z0p2-*.elf

NB the Sinclair 128K bases are header-defined C++ arrays with INTERNAL linkage, so in
`nm` output they carry a `_ZL<len><name>` prefix — that mangling is exactly what the
pointer-keyed overlay registry would fail to match if a second TU ever got its own
copy, which is why they are looked up both ways here.
"""
import os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rom_pack import apply_overlay, GMX_SRC

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLASH_BASE = 0x10000000


def tool(name):
    for d in sorted(__import__('glob').glob(os.path.expanduser('~/.pico-sdk/toolchain/*/bin'))):
        p = os.path.join(d, 'arm-none-eabi-' + name)
        if os.path.exists(p):
            return p
    return 'arm-none-eabi-' + name          # hope it is on PATH


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    elf = sys.argv[1]

    syms = {}
    for line in subprocess.run([tool('nm'), '-S', '-td', elf],
                               capture_output=True, text=True, check=True).stdout.splitlines():
        f = line.split()
        if len(f) != 4:
            continue
        addr, size, _typ, name = f
        syms.setdefault(name, int(addr))
        m = re.match(r'_ZL\d+(\w+)$', name)
        if m:
            syms.setdefault(m.group(1), int(addr))

    raw = elf + '.gmxcheck.bin'
    subprocess.run([tool('objcopy'), '-O', 'binary', elf, raw], check=True)
    blob = open(raw, 'rb').read()
    os.remove(raw)

    def fetch(sym, n=None):
        if sym not in syms:
            raise SystemExit("symbol %s is not in %s" % (sym, elf))
        off = syms[sym] - FLASH_BASE
        return blob[off:off + n] if n else blob[off:]

    tbl = open(os.path.join(ROOT, 'src/roms/scorpion/scorpion_gmx_banks.h')).read()
    rows = re.findall(r'\{\s*(\w+)\s*,\s*(\w+)\s*\}\s*,\s*//\s*plane', tbl)
    if len(rows) != 32:
        raise SystemExit("scorpion_gmx_banks.h: %d rows, want 32" % len(rows))

    got = b''
    for data_sym, ovl_sym in rows:
        data = fetch(data_sym, 16384)
        got += data if ovl_sym == 'nullptr' else apply_overlay(data, fetch(ovl_sym))

    want = open(os.path.join(ROOT, 'src/roms/scorpion/src', GMX_SRC), 'rb').read()
    if got == want:
        print("ELF reconstruction OK: %d B byte-identical to %s" % (len(got), GMX_SRC))
        return 0
    bad = sum(1 for a, b in zip(got, want) if a != b)
    print("ELF reconstruction FAILED: %d of %d bytes differ" % (bad, len(want)))
    for i in range(32):
        a = got[i * 16384:(i + 1) * 16384]
        b = want[i * 16384:(i + 1) * 16384]
        if a != b:
            print("  plane %d bank %d: %d bytes differ  (%s, %s)"
                  % (i // 4, i % 4, sum(1 for x, y in zip(a, b) if x != y), *rows[i]))
    return 1


if __name__ == '__main__':
    sys.exit(main())
