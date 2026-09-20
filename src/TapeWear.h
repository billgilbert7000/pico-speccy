/*

pico-speccy — worn tape model ("the recorder is chewing the tape")

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#ifndef TapeWear_h
#define TapeWear_h

#include <inttypes.h>

// The arithmetic of a stretched, creased, oxide-shedding cassette played on a
// deck whose capstan no longer holds speed. It depends on NOTHING from the
// firmware — that is the only reason tools/tapewear_test.cpp can drive it on a
// host. Tape.cpp owns the one instance, feeds it the pulse the tape state
// machine just chose, and does the two things the model cannot: freeze the ear
// bit and read Config.
//
// Three faults, because each one is a different thing the user remembers:
//
//   wow    the speed wanders, so every pulse comes out a little long or short.
//          The tape signal feeds the speaker, so this is AUDIBLE as the warble
//          everyone remembers — and on its own it is nearly harmless to the
//          loader, exactly as on real hardware.
//   lurch  the tape binds for a moment and the pulses stretch 1.5-3x. The ROM's
//          LD-EDGE classifier then reads a 0 bit as a 1, the block's parity byte
//          disagrees, and LD-BYTES returns with carry clear: "R Tape loading
//          error".
//   drop   a crease or a bald patch lifts the tape off the head: no signal at
//          all for a few milliseconds. Either LD-EDGE's own counter times out,
//          or the frozen level swallows whole bits. Same message either way.

namespace tapewear {

// T-states (3.5 MHz): 3500000 = one second of tape.
static const uint32_t WOW_STEP_T  = 3500;     // one wow step per ~1 ms of tape
static const uint32_t MAX_PULSE_T = 175000;   // 50 ms — longer is a pause, not signal

enum Fault : uint8_t { FAULT_NONE = 0, FAULT_DROP = 1, FAULT_LURCH = 2 };

struct Level {
    int32_t  wowMax;   // speed excursion, 1/256ths (26 = ~10%)
    int32_t  wowStep;  // per-step random walk, same units
    uint32_t evtMin;   // T-states between faults: minimum...
    uint32_t evtSpan;  // ...plus up to this much
    uint32_t durMin;   // fault length in T-states
    uint32_t durSpan;
};

//  wow ±        step   one fault per              lasting
static const Level kLevels[3] = {
    {  10,  1,  35000000u, 35000000u,   1750u,   7000u }, // Light:  ±4%, 10-20 s,   0.5-2.5 ms
    {  20,  2,   7000000u, 10500000u,   7000u,  28000u }, // Medium: ±8%, 2-5 s,     2-10 ms
    {  40,  3,   1750000u,  3500000u,  17500u,  87500u }, // Heavy: ±16%, 0.5-1.5 s, 5-30 ms
};

struct State {
    uint8_t  level    = 0;      // 0 off, 1 Light, 2 Medium, 3 Heavy
    uint32_t rng      = 1;
    int32_t  wow      = 0;      // current speed offset, 1/256ths
    int32_t  wowNext  = 0;      // T-states until the next wow step
    int32_t  evtNext  = 0;      // T-states until the next fault
    uint32_t evtLeft  = 0;      // T-states remaining in the running fault
    uint8_t  evtKind  = FAULT_NONE;
    uint16_t evtMul   = 256;    // lurch stretch, 1/256ths

    uint32_t rand() {           // xorshift32
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }

    void arm() {
        wow = 0;
        wowNext = (int32_t)WOW_STEP_T;
        evtLeft = 0;
        evtKind = FAULT_NONE;
        evtNext = level ? (int32_t)(kLevels[level - 1].evtMin +
                                    rand() % kLevels[level - 1].evtSpan)
                        : 0;
    }

    // Rewinding and pressing play again lands the head on a different part of the
    // damage, so every attempt is a new roll of the dice — which is the point.
    // "Try once more and it might load" is the memory being emulated.
    void reset(uint8_t lvl, uint32_t seed) {
        level = lvl > 3 ? 3 : lvl;
        rng   = seed | 1u;
        arm();
    }

    // The level is read live so a menu change reaches the tape already playing;
    // a change re-arms the schedule rather than leaving the old one running.
    bool sync(uint8_t lvl) {
        if (lvl > 3) lvl = 3;
        if (lvl != level) { level = lvl; arm(); }
        return level != 0;
    }

    // Advance the schedule by `t` T-states of tape travel and start a fault when
    // one comes due. `signalPhase` is false in a pause or tail, where a fault is
    // inaudible and invisible — the tape moves, but there is nothing recorded on
    // it to damage, and a single 1-second pause pulse would otherwise swallow the
    // whole event budget in one step.
    void advance(uint32_t t, bool signalPhase) {
        if (!level) return;
        if (evtKind) {
            evtLeft = (evtLeft > t) ? evtLeft - t : 0;
            if (!evtLeft) evtKind = FAULT_NONE;
            return;
        }
        if (!signalPhase) return;
        evtNext -= (int32_t)t;
        if (evtNext > 0) return;

        const Level& L = kLevels[level - 1];
        evtKind = (rand() & 1) ? FAULT_DROP : FAULT_LURCH;
        evtLeft = L.durMin + rand() % L.durSpan;
        evtMul  = (uint16_t)(384 + rand() % 385);   // 1.5x .. 3.0x
        evtNext = (int32_t)(L.evtMin + rand() % L.evtSpan);
    }

    // Takes the pulse length the tape machine chose and returns the one a worn
    // tape actually delivers. `freeze` comes back true while the head has no tape
    // against it: the caller holds the ear bit where it was, so the loader sees no
    // edge at all.
    uint32_t pulse(uint32_t next, bool signalPhase, bool& freeze) {
        freeze = false;
        if (!level || next == 0 || next > MAX_PULSE_T) return next;

        const Level& L = kLevels[level - 1];

        // Speed wander: a bounded random walk on a clock of its own, slower than
        // the pulse rate — real flutter is a few Hz, not per-pulse jitter.
        wowNext -= (int32_t)next;
        while (wowNext <= 0) {
            wow += (int32_t)(rand() % (uint32_t)(2 * L.wowStep + 1)) - L.wowStep;
            if (wow >  L.wowMax) wow =  L.wowMax;
            if (wow < -L.wowMax) wow = -L.wowMax;
            wowNext += (int32_t)WOW_STEP_T;
        }

        uint32_t out = (uint32_t)((int32_t)next + (int32_t)next * wow / 256);
        if (out == 0) out = 1;

        advance(out, signalPhase);

        if (evtKind == FAULT_LURCH) {
            out = (uint32_t)(((uint64_t)out * evtMul) >> 8);
            if (out > MAX_PULSE_T) out = MAX_PULSE_T;
        } else if (evtKind == FAULT_DROP) {
            freeze = true;
        }
        return out ? out : 1;
    }

    // WAV/MP3: driven by elapsed guest time rather than by pulses, and dropouts
    // only — a wow on a recorded waveform would mean resampling it, and those
    // files are recordings of a real tape anyway, so whatever wear is on them is
    // already there. Both fault kinds present as a dropout here.
    bool audio(uint32_t dt) {
        if (!level) return false;
        if (dt > MAX_PULSE_T) dt = 0;   // first call, or the menu held the machine
        if (dt) advance(dt, true);
        return evtKind != FAULT_NONE;
    }
};

} // namespace tapewear

#endif
