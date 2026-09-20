// Host test for src/TapeWear.h — the worn-tape model behind
// Storage > Tape > Tape wear (Config::tape_wear).
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/tapewear_test tools/tapewear_test.cpp && /tmp/tapewear_test
//
// Re-run after ANY change to TapeWear.h. The model is the only part of the
// feature that can be checked without a Spectrum in front of you, and it fails
// by DEGREES — a tape that is a little too worn still looks like it works.
//
// The last group is the one that matters: a synthetic 48K ROM LD-BYTES decoder
// fed by the model. It is what proves the feature does what was asked for (Heavy
// ends in a loading error), that Off changes nothing at all, and that the wow
// alone — the part that is only meant to be AUDIBLE — never corrupts a byte.

#include "TapeWear.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace tapewear;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

// ── The tape side: a standard ROM block as pulses ─────────────────────────────
static const uint32_t BIT0 = 855, BIT1 = 1710;

// ── The machine side: the 48K ROM's LD-BYTES bit classifier ───────────────────
//
// LD-BYTES measures a bit as two edges (LD-EDGE-2) and compares the total against
// a threshold half way between a 0 (2 x 855) and a 1 (2 x 1710). An edge that
// never arrives inside LD-EDGE's own counter window (~4 ms) is the timeout that
// prints "R Tape loading error" on the spot; a frozen level simply merges pulses,
// which walks the bit stream out of step and fails at the parity byte instead.
struct RomLoader {
    static const uint32_t THRESHOLD = (2 * BIT0 + 2 * BIT1) / 2;   // 2565 T
    static const uint32_t EDGE_TIMEOUT = 15000;                    // ~4.3 ms

    bool timedOut = false;
    uint32_t pending = 0;
    int half = 0;
    uint32_t t1 = 0;
    int nbits = 0;
    uint8_t cur = 0;
    std::vector<uint8_t> out;

    void pulse(uint32_t len, bool freeze) {
        pending += len;
        if (freeze) return;                 // the head lost the tape: no edge
        const uint32_t iv = pending;
        pending = 0;
        if (iv > EDGE_TIMEOUT) { timedOut = true; return; }
        if (half == 0) { t1 = iv; half = 1; return; }
        half = 0;
        const int bit = (t1 + iv) > THRESHOLD ? 1 : 0;
        cur = (uint8_t)((cur << 1) | bit);
        if (++nbits == 8) { out.push_back(cur); cur = 0; nbits = 0; }
    }
};

// Play `data` through a worn tape into a ROM loader. Returns true when the block
// loaded exactly — which is what LD-BYTES' parity byte decides on real hardware.
static bool playBlock(State& w, const std::vector<uint8_t>& data, int* faultsSeen = nullptr) {
    RomLoader rom;
    int faults = 0;
    uint8_t wasFault = FAULT_NONE;
    for (size_t i = 0; i < data.size(); i++) {
        for (int b = 7; b >= 0; b--) {
            const uint32_t len = (data[i] >> b) & 1 ? BIT1 : BIT0;
            for (int halfPulse = 0; halfPulse < 2; halfPulse++) {
                bool freeze = false;
                const uint32_t out = w.pulse(len, true, freeze);
                if (w.evtKind && !wasFault) faults++;
                wasFault = w.evtKind;
                rom.pulse(out, freeze);
            }
        }
    }
    if (faultsSeen) *faultsSeen = faults;
    if (rom.timedOut) return false;
    return rom.out.size() == data.size() &&
           memcmp(rom.out.data(), data.data(), data.size()) == 0;
}

static std::vector<uint8_t> makeBlock(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v[i] = (uint8_t)(seed >> 16); }
    return v;
}

int main() {
    // ── Off is a pure pass-through ────────────────────────────────────────────
    {
        State w; w.reset(0, 12345);
        bool anyFreeze = false, anyChange = false;
        uint32_t rng = 7;
        for (int i = 0; i < 200000; i++) {
            rng = rng * 1103515245u + 12345u;
            const uint32_t in = 100 + (rng >> 20);
            bool freeze = false;
            if (w.pulse(in, true, freeze) != in) anyChange = true;
            if (freeze) anyFreeze = true;
        }
        check(!anyChange, "level 0 changes a pulse length");
        check(!anyFreeze, "level 0 freezes the ear bit");
        check(w.evtKind == FAULT_NONE, "level 0 starts a fault");
    }

    // ── A pulse longer than the cap is a pause, and passes through ────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        State w; w.reset(lvl, 99 * lvl);
        bool freeze = false;
        const uint32_t big = MAX_PULSE_T + 1;
        check(w.pulse(big, true, freeze) == big, "an over-cap pulse is perturbed");
        check(!freeze, "an over-cap pulse freezes the ear bit");
        check(w.pulse(0, true, freeze) == 0, "a zero pulse is perturbed");
    }

    // ── A delivered pulse is never zero, and never over the cap ───────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        State w; w.reset(lvl, 4242 + lvl);
        for (int i = 0; i < 400000; i++) {
            bool freeze = false;
            const uint32_t out = w.pulse(BIT0, true, freeze);
            check(out > 0, "a delivered pulse is zero");
            if (out > MAX_PULSE_T) { check(false, "a delivered pulse exceeds the cap"); break; }
        }
    }

    // ── The speed wander stays inside its bound, both ways ────────────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        const Level& L = kLevels[lvl - 1];
        State w; w.reset(lvl, 777 * lvl);
        int32_t lo = 0, hi = 0;
        for (int i = 0; i < 2000000; i++) {
            bool freeze = false;
            w.pulse(BIT1, true, freeze);
            if (w.wow < lo) lo = w.wow;
            if (w.wow > hi) hi = w.wow;
        }
        check(lo >= -L.wowMax && hi <= L.wowMax, "the wow walk left its bound");
        // ...and it is a walk, not a constant: a level with a bound must use it.
        check(hi > L.wowMax / 2 && lo < -L.wowMax / 2, "the wow walk never moves");
    }

    // ── Faults come at about the configured rate, and last the right time ─────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        const Level& L = kLevels[lvl - 1];
        State w; w.reset(lvl, 31337 + lvl);
        const uint64_t tapeT = 600ull * 3500000ull;     // ten minutes of tape
        uint64_t played = 0;
        int faults = 0;
        uint8_t was = FAULT_NONE;
        uint32_t started = 0;
        bool durOk = true, mulOk = true;
        while (played < tapeT) {
            bool freeze = false;
            const uint32_t out = w.pulse(BIT0, true, freeze);
            played += out;
            if (w.evtKind && !was) {
                faults++;
                started = w.evtLeft;
                if (started < L.durMin || started >= L.durMin + L.durSpan) durOk = false;
                // A scheduled lurch must actually stretch: 1.5x-3.0x. (The
                // per-kind test above sets evtMul by hand, so this is the only
                // place the scheduler's own choice is checked.)
                if (w.evtKind == FAULT_LURCH && (w.evtMul < 384 || w.evtMul > 768)) mulOk = false;
            }
            was = w.evtKind;
        }
        const double expect = (double)tapeT / (L.evtMin + L.evtSpan / 2.0);
        check(faults > expect * 0.5 && faults < expect * 2.0, "the fault rate is off");
        check(durOk, "a fault length left [durMin, durMin+durSpan)");
        check(mulOk, "a scheduled lurch does not stretch by 1.5x-3.0x");
    }

    // ── Nothing is damaged where nothing is recorded ──────────────────────────
    {
        State w; w.reset(3, 5150);
        for (int i = 0; i < 400000; i++) {
            bool freeze = false;
            w.pulse(BIT0, false, freeze);      // a pause/tail, every pulse
            check(w.evtKind == FAULT_NONE || i < 0, "a fault started in a pause");
            if (w.evtKind) break;
        }
    }

    // ── Off loads a block byte for byte ───────────────────────────────────────
    {
        const std::vector<uint8_t> blk = makeBlock(6912, 1);
        State w; w.reset(0, 1);
        check(playBlock(w, blk), "level 0 fails to load a block");
    }

    // ── The wow ALONE never corrupts a byte ───────────────────────────────────
    //
    // ±16% (Heavy) keeps a 0 bit at 2 x 1710 x 1.16 = 3968 and a 1 bit at
    // 2 x 3420 x 0.84 = 5745 — both well clear of the 5130 T threshold. This is
    // the property that makes the warble a SOUND and not a failure, so it is
    // asserted rather than assumed: every run that drew no fault must load.
    {
        // Heavy's fault schedule would fire inside any block long enough to be
        // worth decoding, so the fault is held off explicitly: this is the wow on
        // its own, at the widest excursion the feature can produce.
        int bad = 0;
        for (uint32_t seed = 1; seed <= 200; seed++) {
            const std::vector<uint8_t> blk = makeBlock(400, seed);
            State w; w.reset(3, seed * 2654435761u);
            RomLoader rom;
            for (size_t i = 0; i < blk.size(); i++)
                for (int b = 7; b >= 0; b--) {
                    const uint32_t len = (blk[i] >> b) & 1 ? BIT1 : BIT0;
                    for (int h = 0; h < 2; h++) {
                        w.evtNext = 0x40000000;          // no fault, wow only
                        bool freeze = false;
                        // Order matters: `freeze` is an out-parameter, and the two
                        // arguments of a call are evaluated in unspecified order.
                        const uint32_t got = w.pulse(len, true, freeze);
                        rom.pulse(got, freeze);
                    }
                }
            if (rom.timedOut || rom.out.size() != blk.size() ||
                memcmp(rom.out.data(), blk.data(), blk.size()) != 0) bad++;
        }
        check(bad == 0, "the wow alone corrupted a block");
    }

    // ── ...and EACH KIND of fault does, on its own ────────────────────────────
    //
    // Per kind, and injected by hand, because a run of Heavy draws both: with the
    // two mixed, neutering either one still leaves the other to fail the block —
    // which is a suite that cannot fail. The control run (same seed, no fault) has
    // to load, or the test would be proving nothing about the fault.
    {
        for (int kind = FAULT_DROP; kind <= FAULT_LURCH; kind++) {
            int broke = 0, controlOk = 0;
            for (uint32_t seed = 1; seed <= 50; seed++) {
                for (int inject = 0; inject < 2; inject++) {
                    const std::vector<uint8_t> blk = makeBlock(200, seed);
                    State w; w.reset(3, seed * 69069u + 1);
                    RomLoader rom;
                    int pulses = 0;
                    for (size_t i = 0; i < blk.size(); i++)
                        for (int b = 7; b >= 0; b--) {
                            const uint32_t len = (blk[i] >> b) & 1 ? BIT1 : BIT0;
                            for (int h = 0; h < 2; h++) {
                                w.evtNext = 0x40000000;        // schedule off: this is a hand-placed fault
                                if (inject && pulses == 800) {
                                    w.evtKind = (uint8_t)kind;
                                    w.evtLeft = 10000;         // ~2.9 ms
                                    w.evtMul  = 512;           // 2x, for the lurch
                                }
                                pulses++;
                                bool freeze = false;
                                const uint32_t got = w.pulse(len, true, freeze);
                                rom.pulse(got, freeze);
                            }
                        }
                    const bool ok = !rom.timedOut && rom.out.size() == blk.size() &&
                                    memcmp(rom.out.data(), blk.data(), blk.size()) == 0;
                    if (inject) { if (!ok) broke++; } else if (ok) controlOk++;
                }
            }
            check(controlOk == 50, "the control run of the per-kind test does not load");
            // A dropout always costs bits, so it always breaks the block. A lurch
            // only breaks a 0 bit (stretched past the threshold it reads as a 1) —
            // over a run of 1s it is inaudible to the loader, which is right, so it
            // is a handful of the 50 seeds and not none of them.
            if (kind == FAULT_DROP) check(broke == 50, "a dropout let a block through intact");
            else                    check(broke >= 40, "a lurch let too many blocks through intact");
        }
    }

    // ── ...and a fault always does ────────────────────────────────────────────
    //
    // Both kinds are meant to end in the ROM's own message: a lurch stretches a 0
    // past the threshold, a dropout merges pulses until the stream is out of step.
    {
        int faulted = 0, faultedOk = 0;
        for (uint32_t seed = 1; seed <= 200; seed++) {
            const std::vector<uint8_t> blk = makeBlock(2000, seed);
            State w; w.reset(3, seed * 40503u + 7);
            int faults = 0;
            const bool ok = playBlock(w, blk, &faults);
            if (faults > 0) { faulted++; if (ok) faultedOk++; }
        }
        check(faulted > 150, "Heavy hardly ever damages a 2000-byte block");
        check(faultedOk == 0, "a fault went through without a loading error");
    }

    // ── The levels mean what their labels say ─────────────────────────────────
    {
        // Light, a small loader block (~1.2 s of tape): usually loads.
        int ok = 0;
        for (uint32_t seed = 1; seed <= 100; seed++) {
            const std::vector<uint8_t> blk = makeBlock(200, seed);
            State w; w.reset(1, seed * 1664525u + 1013904223u);
            if (playBlock(w, blk)) ok++;
        }
        check(ok >= 80, "Light breaks a small block too often");

        // Medium, a full screen (~40 s of tape): essentially never loads.
        ok = 0;
        for (uint32_t seed = 1; seed <= 50; seed++) {
            const std::vector<uint8_t> blk = makeBlock(6912, seed);
            State w; w.reset(2, seed * 22695477u + 1);
            if (playBlock(w, blk)) ok++;
        }
        check(ok <= 5, "Medium loads a whole screen too often");
    }

    // ── A level change re-arms rather than carrying the old schedule ──────────
    {
        State w; w.reset(1, 8);
        for (int i = 0; i < 10000; i++) { bool f = false; w.pulse(BIT0, true, f); }
        const int32_t before = w.evtNext;
        check(w.sync(3), "sync(3) reported the tape as unworn");
        check(w.level == 3, "sync did not take the new level");
        check(w.evtNext != before, "a level change kept the old schedule");
        check(!w.sync(0), "sync(0) reported the tape as worn");
        bool f = false;
        check(w.pulse(BIT0, true, f) == BIT0, "level 0 after a change still perturbs");
    }

    // ── The WAV/MP3 path: dropouts on elapsed time ────────────────────────────
    {
        State w; w.reset(3, 4711);
        check(!w.audio(0), "an idle audio tick reported a fault");
        int faults = 0;
        uint8_t was = FAULT_NONE;
        for (int i = 0; i < 200000; i++) {          // 200000 x 79 T ~ 4.5 s
            w.audio(79);
            if (w.evtKind && !was) faults++;
            was = w.evtKind;
        }
        check(faults > 0, "the audio path never drops out");
        State off; off.reset(0, 1);
        check(!off.audio(79), "level 0 drops out on the audio path");
        // A gap longer than the cap (the menu held the machine) must not count.
        State g; g.reset(3, 22);
        const int32_t evt = g.evtNext;
        g.audio(MAX_PULSE_T + 1);
        check(g.evtNext == evt, "a long audio gap spent the fault budget");
    }

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("tapewear_test: OK\n");
    return 0;
}
