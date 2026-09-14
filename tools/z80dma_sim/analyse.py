#!/usr/bin/env python3
"""Replay z80dma_sim's dma.log for one frame against three attribute models and
render each as PNG: hw.png (live memory sampled at the beam, i.e. what a real
Pentagon shows), ours.png (pico-speccy's per-scanline DMA attr shadow as it was
before 2026-09-14: shadow only once the content changed), fixed.png (shadow from
the second write unconditionally — the shipped rule) and intended.png (scanline
8c+k = the k-th DMA into charrow c). Prints the mismatch counts and the lines.
    python3 analyse.py <snapdir> <frame>     (dma.log in the current directory)
"""
import sys
from PIL import Image
SNAP=sys.argv[1] if len(sys.argv)>1 else '.'
F=int(sys.argv[2]) if len(sys.argv)>2 else 4
TL0=17983; LINE=224
page5=bytearray(open(SNAP+'/page5.bin','rb').read())
ev=[]  # (T, kind, ...) chronological across frames
cur=None
for l in open('dma.log'):
    p=l.split()
    if p[0]=='F': cur=int(p[1]); continue
    if p[0]=='W': ev.append((int(p[1]),int(p[2]),'W',int(p[3],16),int(p[4],16),int(p[5])))
    elif p[0]=='D': ev.append((int(p[1]),int(p[3]),'D',int(p[2]),int(p[4],16),int(p[5],16),int(p[6])))
ev.sort(key=lambda e:(e[0],e[1]))
attr=page5[0x1800:0x1B00]
# replay frames < F
for e in ev:
    if e[0]<F and e[2]=='W':
        a=e[3]-0x4000
        if 0x1800<=a<0x1B00: attr[a-0x1800]=e[4]
attr0=bytes(attr)
fev=[e for e in ev if e[0]==F]
def Tline(y): return TL0+LINE*y
# hardware view
def build_hw():
    img=[[0]*32 for _ in range(192)]
    a=bytearray(attr0); wi=0
    W=[e for e in fev if e[2]=='W']
    for y in range(192):
        for c in range(32):
            t=Tline(y)+4*c
            while wi<len(W) and W[wi][1]<=t:
                ad=W[wi][3]-0x4000
                if 0x1800<=ad<0x1B00: a[ad-0x1800]=W[wi][4]
                wi+=1
            img[y][c]=a[(y>>3)*32+c]
    return img
# pico-speccy view with the shadow heuristic
def build_ours(unconditional):
    img=[[None]*32 for _ in range(192)]
    a=bytearray(attr0)
    shadow=[[0]*32 for _ in range(192)]; valid=[False]*192
    prev=[None]*24; cnt=[0]*24; active=[False]*24
    # events in time order; line-start decisions at Tline(y)
    W=[e for e in fev if e[2]=='W']; D=[e for e in fev if e[2]=='D']
    # merge: apply writes; at each D end run capture; at each line start decide override
    marks=[('L',Tline(y),y) for y in range(192)]
    allev=[('W',e[1],e) for e in W]+[('D',e[1],e) for e in D]+marks
    # for live rendering we need per column; approximate: decide override at line start; columns read at Tline+4c from live memory
    allev.sort(key=lambda x:(x[1], 0 if x[0]=='W' else 1 if x[0]=='D' else 2))
    override=[None]*192
    colq=[]  # (t,y,c)
    for y in range(192):
        for c in range(32): colq.append((Tline(y)+4*c,y,c))
    qi=0
    def flush_cols(upto):
        nonlocal qi
        while qi<len(colq) and colq[qi][0]<upto:
            t,y,c=colq[qi]; qi+=1
            if override[y] is not None: img[y][c]=shadow[y][c]
            else: img[y][c]=a[(y>>3)*32+c]
    for kind,t,e in allev:
        flush_cols(t)  # columns strictly before this event time
        if kind=='W':
            ad=e[3]-0x4000
            if 0x1800<=ad<0x1B00: a[ad-0x1800]=e[4]
        elif kind=='D':
            dst=e[5]
            if not (0x5800<=dst<=0x5AFF): continue
            cr=(dst-0x5800)>>5
            sub=cnt[cr]
            if sub>=8: continue
            cnt[cr]=sub+1
            sl=cr*8+sub
            base=cr*32
            if sub==0:
                prev[cr]=bytes(a[base:base+32]); continue
            if prev[cr] is None: continue
            if not active[cr]:
                if not unconditional and bytes(a[base:base+32])==prev[cr]:
                    prev[cr]=bytes(a[base:base+32]); continue
                active[cr]=True
                shadow[cr*8]=list(prev[cr]); valid[cr*8]=True
            ps=cr*8+sub-1
            shadow[ps]=list(prev[cr]); valid[ps]=True
            shadow[sl]=list(a[base:base+32]); valid[sl]=True
            prev[cr]=bytes(a[base:base+32])
        else:
            y=e
            override[y]=True if valid[y] else None
    flush_cols(10**9)
    return img
hw=build_hw(); ours=build_ours(False); fixed=build_ours(True)
def diff(a,b): return sum(1 for y in range(192) for c in range(32) if a[y][c]!=b[y][c])
print('mismatch ours vs hw:',diff(ours,hw),' fixed vs hw:',diff(fixed,hw))
# per-line report where ours differs
bad=[y for y in range(192) if any(ours[y][c]!=hw[y][c] for c in range(32))]
print('lines differing (ours):',bad[:40], '...' if len(bad)>40 else '')
badf=[y for y in range(192) if any(fixed[y][c]!=hw[y][c] for c in range(32))]
print('lines differing (fixed):',badf[:40])
pal=[(0,0,0),(0,0,0xD8),(0xD8,0,0),(0xD8,0,0xD8),(0,0xD8,0),(0,0xD8,0xD8),(0xD8,0xD8,0),(0xD8,0xD8,0xD8)]
def render(img,name):
    im=Image.new('RGB',(256,192))
    px=im.load()
    for y in range(192):
        for c in range(32):
            at=img[y][c] or 0
            ink=pal[at&7]; pap=pal[(at>>3)&7]
            if at&0x40: ink=tuple(min(255,v+39) for v in ink); pap=tuple(min(255,v+39) for v in pap)
            for x in range(8): px[c*8+x,y]= ink if x<4 else pap
    im.resize((512,384),Image.NEAREST).save(name)
render(hw,'hw.png'); render(ours,'ours.png'); render(fixed,'fixed.png')

# intended picture: line 8c+k = attr row content right after the k-th DMA into charrow c this frame
def build_intended():
    img=[[0]*32 for _ in range(192)]
    a=bytearray(attr0); cnt=[0]*24
    W=[e for e in fev if e[2]=='W']; D=[e for e in fev if e[2]=='D']
    allev=sorted([('W',e[1],e) for e in W]+[('D',e[1],e) for e in D], key=lambda x:(x[1],0 if x[0]=='W' else 1))
    for kind,t,e in allev:
        if kind=='W':
            ad=e[3]-0x4000
            if 0x1800<=ad<0x1B00: a[ad-0x1800]=e[4]
        else:
            dst=e[5]
            if not (0x5800<=dst<=0x5AFF): continue
            cr=(dst-0x5800)>>5; k=cnt[cr]; cnt[cr]+=1
            if k<8: img[cr*8+k]=list(a[cr*32:cr*32+32])
    return img
intended=build_intended()
print('mismatch ours vs intended:',diff(ours,intended),' fixed vs intended:',diff(fixed,intended))
print('lines differing (ours vs intended):',[y for y in range(192) if any(ours[y][c]!=intended[y][c] for c in range(32))])
print('lines differing (fixed vs intended):',[y for y in range(192) if any(fixed[y][c]!=intended[y][c] for c in range(32))])
render(intended,'intended.png')
