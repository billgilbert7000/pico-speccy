#!/usr/bin/env python3
"""Frame model for a guest that DMAs attributes into one screen page while the
ULA displays the other (NaPICu's "DMA DEZIGN" title, 2026-09-14). Needs a
z80dma_sim run that logged frame F (D lines with data) and dumped its pages
(argument 6 = F): the displayed page per T-state comes from the P lines, page 7
from the dump, page 5's attribute rows from the DMA log. Renders four rules:
  hw        live attrs + bitmap of the displayed page (what a real 128K shows)
  old       the pre-2026-09-14 shadow (shadow only once a row CHANGED, read via grmem)
  grmem     unconditional shadow read through grmem — the bug: page 7's white
            0x07 rows become the shadow, and the frame tail displayed from page 5
            renders its 0xF0 bitmap fill in white (the comb under the title)
  fixed     shadow taken from the written page, applied only while it is displayed
    python3 analyse_pages.py <dumpdir> <frame> [pentagon|128k]
"""
import sys
from PIL import Image
D, F = sys.argv[1], int(sys.argv[2])
mach = sys.argv[3] if len(sys.argv) > 3 else 'pentagon'
TL0, LINE = (17983, 224) if mach == 'pentagon' else (14361, 228)
p5 = bytearray(open(f'{D}/dump_f{F}_page5.bin', 'rb').read()); p7 = open(f'{D}/dump_f{F}_page7.bin', 'rb').read()
attr5 = bytearray(p5[0x1800:0x1B00])          # page 5 attrs at frame start (dump is taken at the INT)
P, Dm = [], []
for l in open('dma.log'):
    p = l.split()
    if p[0] == 'P' and int(p[1]) == F: P.append((int(p[2]), (int(p[3], 16) >> 3) & 1))
    if p[0] == 'D' and int(p[1]) == F: Dm.append((int(p[3]), int(p[5], 16), bytes.fromhex(p[7])))
P.sort(); Dm.sort()
def page_at(t):
    pg = 0
    for (tt, v) in P:
        if tt <= t: pg = v
    return pg
def attrs5_at(t):
    a = bytearray(attr5)
    for (te, dst, data) in Dm:
        if te <= t and 0x5800 <= dst <= 0x5AFF: a[dst - 0x5800:dst - 0x5800 + 32] = data
    return a
def Tline(y): return TL0 + LINE * y
def render(rule):
    img = Image.new('RGB', (256, 192)); px = img.load()
    pal = [(0, 0, 0), (0, 0, 0xD8), (0xD8, 0, 0), (0xD8, 0, 0xD8), (0, 0xD8, 0), (0, 0xD8, 0xD8), (0xD8, 0xD8, 0), (0xD8, 0xD8, 0xD8)]
    # shadow per rule: (valid[y], row[y])
    shadow = {}; cnt = [0] * 24; prev = [None] * 24; active = [False] * 24
    for (te, dst, data) in Dm:
        if not (0x5800 <= dst <= 0x5AFF): continue
        cr = (dst - 0x5800) >> 5; sub = cnt[cr]; cnt[cr] += 1
        if sub >= 8: continue
        pg = page_at(te)
        src = (p7[0x1800 + cr * 32:0x1800 + cr * 32 + 32] if pg else bytes(attrs5_at(te)[cr * 32:cr * 32 + 32])) if rule in ('old', 'grmem') else bytes(attrs5_at(te)[cr * 32:cr * 32 + 32])
        if sub == 0: prev[cr] = src; continue
        if rule == 'old' and not active[cr]:
            if src == prev[cr]: prev[cr] = src; continue
            active[cr] = True
        shadow[cr * 8 + sub - 1] = prev[cr]; shadow[cr * 8 + sub] = src; prev[cr] = src
    for y in range(192):
        t0 = Tline(y); pg = page_at(t0)
        use_sh = rule != 'hw' and y in shadow and (rule != 'fixed' or pg == 0)
        for c in range(32):
            t = t0 + 4 * c; pgc = page_at(t)
            src_page = p7 if pgc else p5
            off = ((y & 7) << 8) | ((y >> 3 & 7) << 5) | ((y >> 6) << 11)
            b = src_page[off + c]
            a = shadow[y][c] if use_sh else (p7[0x1800 + (y >> 3) * 32 + c] if pgc else attrs5_at(t)[(y >> 3) * 32 + c])
            ink = pal[a & 7]; pap = pal[(a >> 3) & 7]
            for x in range(8): px[c * 8 + x, y] = ink if (b >> (7 - x)) & 1 else pap
    return img
sheet = Image.new('RGB', (4 * 266, 200), (60, 60, 60))
for i, r in enumerate(('hw', 'old', 'grmem', 'fixed')):
    im = render(r); sheet.paste(im, (i * 266, 0))
    nonblack = sum(1 for y in range(140, 192) for x in range(0, 256, 8) if im.getpixel((x, y)) != (0, 0, 0))
    print(f'{r:6s}: lit pixels sampled in lines 140-191 = {nonblack}')
sheet.save('pages_sheet.png'); print('page switches:', P)
