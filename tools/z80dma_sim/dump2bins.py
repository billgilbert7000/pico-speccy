#!/usr/bin/env python3
"""Split a Ctrl+Alt+D memory dump (/tmp/picospec_dump.log) into the files the
host simulator z80dma_sim.c loads: mem64.bin (the 64 KB view as the Z80 saw it,
0000-3FFF = the ROM that was paged in), page0..7.bin (the eight 128K RAM pages)
and regs.txt (registers + IM/IFF + the #7FFD latch, one token each).

    python3 tools/z80dma_sim/dump2bins.py /tmp/picospec_dump.log outdir/

Parses hex lines of the form 'XXXX: 16 bytes  |ascii|' (there is a double
space after the 8th byte — split the hex field up to the '|', never with a
'byte + optional space' regex, that captured 8 bytes per line and produced a
disassembly full of NOPs, which cost one round here).
"""
import re, sys, os
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
lines = open(src, errors='replace').read().split('\n')
hexline = re.compile(r'^([0-9A-F]{4}): ([0-9A-F ]+?)\s*\|')
def parse(start, stop):
    mem = bytearray(65536); i = start
    while i < len(lines):
        l = lines[i]
        if i > start and stop(l): break
        m = hexline.match(l)
        if m:
            a = int(m.group(1), 16); bs = bytes.fromhex(m.group(2).replace(' ', ''))
            mem[a:a + len(bs)] = bs
        i += 1
    return mem
i0 = next(i for i, l in enumerate(lines) if l.startswith('--- Memory dump'))
open(os.path.join(out, 'mem64.bin'), 'wb').write(parse(i0 + 1, lambda l: l.startswith('===')))
for p in range(8):
    k = next(i for i, l in enumerate(lines) if l.startswith(f'--- RAM page {p} '))
    open(os.path.join(out, f'page{p}.bin'), 'wb').write(parse(k + 1, lambda l: l.startswith('---') or l.startswith('==='))[:16384])
txt = '\n'.join(lines[:40])
def reg(name): return int(re.search(re.escape(name) + r'[:=]\s*([0-9A-F]+)', txt).group(1), 16)
rom = reg('romLatch'); bank = reg('bankLatch'); vid = reg('videoLatch')
p7ffd = (bank & 7) | (vid << 3) | (rom << 4)
vals = [reg(n) for n in ("AF", "BC", "DE", "HL", "AF'", "BC'", "DE'", "HL'", "IX", "IY", "SP", "PC", "I", "R")]
im = int(re.search(r'IM=(\d)', txt).group(1)); iff1 = int(re.search(r'IFF1=(\d)', txt).group(1)); iff2 = int(re.search(r'IFF2=(\d)', txt).group(1))
open(os.path.join(out, 'regs.txt'), 'w').write(' '.join('%X' % v for v in vals) + ' %d %d %d %02X\n' % (im, iff1, iff2, p7ffd))
print('wrote', out, 'PC=%04X SP=%04X 7FFD=%02X' % (vals[11], vals[10], p7ffd))
