// TS-Conf CPU cache (zmem.v: 256 x 16-bit words, index a[8:1], tag {page,
// a[13:9]}) — the DRAM model's hit test, in the shape the per-access hot path
// pays for. Firmware-free so tools/tsdram_cache_test.cpp can drive it against a
// plain tag model; TsConf.cpp defines the state, the test defines its own.
//
// Representation (2026-09-13, the host-cost session): the truth is still one
// tag per cache index (g_ts_cache_tag), but the per-access test no longer
// derives the expected tag from the window's page — it reads a per-WINDOW ROW,
// tsdc_row[w][idx] = a[13:9] of the word the row's page has cached at idx, or
// 0xFF. So a hit is one byte load and one compare on the address bits alone:
//
//     g_ts_hitrow[addr >> 14][(addr >> 1) & 0xFF] == (addr >> 9) & 0x1F
//
// 8 instructions, two scratch registers, against 11 and a push {r4} for the
// tagbase | tag form (fishbone at 14 MHz lost 3 FPS to it). g_ts_hitrow[w]
// points at the window's row while the window is RAM with its cache enabled
// and at tsdc_none (all 0xFF, never matches) otherwise — ROM in window 0 and a
// CacheConfig toggle are pointer swaps, not rebuilds. g_ts_invrow[w] is the
// same for the WRITE side, where a RAM window invalidates whether or not its
// cache is enabled (cache_inv follows cpu_strobe, not cache_en).
//
// Rows are maintained for the page they were built for (tsdc_row_page[w]),
// independent of whether the window is ROM right now: every tag fill updates
// all four rows, so a ROM->RAM return to the same page needs no rebuild —
// Bomberman brackets every DMA with MEMCONF=04 / PAGE0=0 — and only a real
// page change rebuilds one row (256 entries, ~1500 cycles). Two windows on the
// same page (g_ts_alias) send the inline invalidate to a cold path that clears
// every row, since a hit in one alias is a hit in the other.
#pragma once
#include <stdint.h>
#include <string.h>

extern uint8_t* g_ts_hitrow[4];      // read test: the window's row, or tsdc_none
extern uint8_t* g_ts_invrow[4];      // write invalidate: the row while RAM, else tsdc_none
extern uint16_t g_ts_cache_tag[256]; // 0 = invalid, else 0x8000 | page << 5 | a[13:9]
extern uint16_t g_ts_tagbase[4];     // cold path: 0 = ROM (no DRAM), else 0x8000 | page << 5 [| 0x4000 cache off]
extern uint8_t  g_ts_alias;          // two windows share a page: writes invalidate through tsdcInvCold
extern uint8_t  tsdc_row[4][256];
extern uint8_t  tsdc_none[256];      // all 0xFF
extern uint16_t tsdc_row_page[4];    // page each row is built for, 0xFFFF = none

#define TSDC_INLINE static inline __attribute__((always_inline))

// True when the access costs no DRAM cycle: a hit in an enabled cache window.
// ROM answers false here and 0 in tsdcFill — one cold call per ROM access
// instead of a third load on every access (in the whole-line video modes where
// this path is hot, ROM code is TS-BIOS Setup and nothing else).
TSDC_INLINE bool tsMemNoDram(uint16_t addr) {
    return g_ts_hitrow[addr >> 14][(addr >> 1) & 0xFF] == (uint8_t)((addr >> 9) & 0x1F);
}

void tsdcInvCold(uint16_t addr);     // alias case: drop the tag and every row's entry (TsConf.cpp / the test)

// A CPU write invalidates the cached word (cache_inv) — no wait; the DMA steal
// is the caller's business. Two halves:
//  - tsMemWriteHit: the TEST alone, for the leaf fast paths. A write that hits
//    is the exception (the 256-word cache is swept by every 512 bytes of code,
//    so a word rarely survives from its read to its write), and the leaf
//    tail-calls its cold path on it — the invalidate itself needs the tag
//    array's address plus a zero and a 0xFF in registers, which cost poke8 a
//    push/pop pair on EVERY write when it was inlined (2026-09-13).
//  - tsMemWriteInv: the full rule, alias case included; what the cold paths run.
TSDC_INLINE bool tsMemWriteHit(uint16_t addr) {
    return g_ts_invrow[addr >> 14][(addr >> 1) & 0xFF] == (uint8_t)((addr >> 9) & 0x1F);
}
TSDC_INLINE void tsMemWriteInv(uint16_t addr) {
    if (__builtin_expect(tsMemWriteHit(addr), 0)) {
        if (__builtin_expect(g_ts_alias != 0, 0)) { tsdcInvCold(addr); return; }
        const uint32_t idx = (addr >> 1) & 0xFF;
        g_ts_cache_tag[idx] = 0;
        g_ts_invrow[addr >> 14][idx] = 0xFF;
    }
}

// --- cold side -------------------------------------------------------------

static inline void tsdcInvColdImpl(uint16_t addr) {
    const uint32_t idx = (addr >> 1) & 0xFF;
    g_ts_cache_tag[idx] = 0;
    for (uint32_t w = 0; w < 4; w++) tsdc_row[w][idx] = 0xFF;
}

// The miss: a CPU read of RAM that goes to DRAM fills the tag (cpu_strobe) and
// every row. Returns false for ROM (no DRAM request at all).
static inline bool tsdcFill(uint16_t addr) {
    const uint16_t tb = g_ts_tagbase[addr >> 14];
    if (!tb) return false;
    const uint32_t idx = (addr >> 1) & 0xFF;
    const uint8_t  a   = (uint8_t)((addr >> 9) & 0x1F);
    const uint16_t page = (uint16_t)((tb >> 5) & 0xFF);
    g_ts_cache_tag[idx] = (uint16_t)((tb & ~0x4000u) | a);
    for (uint32_t w = 0; w < 4; w++) tsdc_row[w][idx] = (tsdc_row_page[w] == page) ? a : 0xFF;
    return true;
}

static inline void tsdcRebuildRow(uint32_t w, uint16_t page) {
    const uint16_t want = (uint16_t)(0x8000u | ((uint32_t)page << 5));
    for (uint32_t idx = 0; idx < 256; idx++) {
        const uint16_t t = g_ts_cache_tag[idx];
        tsdc_row[w][idx] = ((t & 0xFFE0u) == want) ? (uint8_t)(t & 0x1F) : 0xFF;
    }
    tsdc_row_page[w] = page;
}

// The bank map / W0_RAM / CacheConfig may have changed. bank_phys[w] = the
// physical page in window w (for a ROM window 0 that is the ROM page number and
// is ignored). Returns the number of rows rebuilt (PERF).
static inline uint32_t tsdcRecalc(const uint8_t bank_phys[4], bool w0_ram, uint8_t cacheconf) {
    uint32_t rebuilt = 0;
    for (uint32_t w = 0; w < 4; w++) {
        const bool ram = (w != 0) || w0_ram;
        const bool en  = (cacheconf >> w) & 1;
        if (!ram) {
            g_ts_tagbase[w] = 0;
            g_ts_hitrow[w] = g_ts_invrow[w] = tsdc_none;
            continue;   // the row keeps serving its old page (see the header comment)
        }
        const uint16_t page = bank_phys[w];
        g_ts_tagbase[w] = (uint16_t)(0x8000u | ((uint32_t)page << 5) | (en ? 0 : 0x4000u));
        if (tsdc_row_page[w] != page) { tsdcRebuildRow(w, page); rebuilt++; }
        g_ts_hitrow[w] = en ? tsdc_row[w] : tsdc_none;
        g_ts_invrow[w] = tsdc_row[w];
    }
    uint8_t alias = 0;
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t j = i + 1; j < 4; j++)
            if (tsdc_row_page[i] != 0xFFFF && tsdc_row_page[i] == tsdc_row_page[j]) alias = 1;
    g_ts_alias = alias;
    return rebuilt;
}

// Power-up / reset: the cache comes up invalid.
static inline void tsdcReset() {
    memset(g_ts_cache_tag, 0, sizeof g_ts_cache_tag);
    memset(tsdc_row, 0xFF, sizeof tsdc_row);
    memset(tsdc_none, 0xFF, sizeof tsdc_none);
    for (uint32_t w = 0; w < 4; w++) { tsdc_row_page[w] = 0xFFFF; g_ts_hitrow[w] = g_ts_invrow[w] = tsdc_none; g_ts_tagbase[w] = 0; }
    g_ts_alias = 0;
}
