# HSTX video backend for m2p2 — analysis and plan

**Status: PLAN, not a record of anything built.** Nothing here has been implemented or
run on hardware. Written 2026-09-16 from a read of the tree, the RP2350 HSTX
documentation, `raspberrypi/pico-examples` and `DnCraptor/quakegeneric`. When this
lands, the confirmed parts belong in CLAUDE.md and this file goes away.

## Goal

Drive the display from the RP2350's **HSTX** serializer instead of PIO, on the
Murmulator 2.0 + Pico 2 board (`m2p2` / CMake target `MURM2`).

Owner's constraints, from the session that produced this plan:

1. **One firmware**, named `m2p2-speccy-VGA-HDMI-HSTX-<ver>.uf2`, exactly the way
   `quakegeneric` does it (`IF(MURM2) if(VGA_HDMI) set(DVI_HSTX ON)` plus a `-HSTX`
   suffix). No extra pair in the build matrix, no second release asset.
2. **Do not split the work into a VGA half and an HDMI half.** One HSTX backend
   serves both outputs and lands as one feature.
3. Other boards keep the PIO path untouched.

## Why only m2p2 (and which other boards could follow)

HSTX exists **only on GPIO 12-19**. Per board:

| board | `HDMI_BASE_PIN` / `VGA_BASE_PIN` | HSTX |
|---|---|---|
| **MURM2 (m2p2)** | 12 -> 12..19 | **yes** |
| PICO_PC (PCp2) | 12 -> 12..19 | yes, electrically — needs its own lane map (`get_ser_diff_data` swaps R/G there under `#ifdef PICO_PC`) |
| MURM2_W (m2p2w) | 12 (inherits the MURM2 arm) | yes |
| MURM1, PICO_DV | 6 | no |
| ZERO2 | 32 | no |

So the real criterion is `HDMI_BASE_PIN == 12`. Write the pin/lane map as a per-board
table from the start; PCp2 and m2p2w then cost one entry each plus a hardware run.

## The two outputs share one pin group, and the choice is boot-time only

| GPIO | VGA today | HDMI today (PIO) | HDMI on HSTX | VGA on HSTX |
|---|---|---|---|---|
| 12, 13 | colour bits 0-1 | CK-/CK+ (side-set) | CK-/CK+ (`bit[].CLK`) | colour bits 0-1 |
| 14-17 | colour bits 2-5 | D0+-, D1+- | lanes 0/1 | colour bits 2-5 |
| 18, 19 | HS, VS (bits 6/7) | D2+- | lane 2 | HS, VS |

1. `main()` -> `testPins(VGA_BASE_PIN, VGA_BASE_PIN+1)` — a passive pull-up/pull-down
   probe of GP12/13 with `gpio_init`/`gpio_deinit`, long before any funcsel.
2. `Config::load()` -> `resolveVideoOutput()` (`video_driver` from NVS, else
   `linkVGA01`) -> `SELECT_VGA`. Needed before `VIDEO::Init` because the framebuffer
   is claimed from the chosen output's video mode.
3. core1 -> `graphics_init()` re-derives the same thing and enters one branch.

There is **no live VGA<->HDMI switch** (`vga_reinit()` runs for VGA modes only; an HDMI
mode change is reboot-class), so the HSTX peripheral only ever has to be configured
once, for whichever output came up. `video_driver` has no menu row today — it is an
NVS string (`auto`/`vga`/`hdmi`); the jumper decides in practice.

## What the two drivers cost today

**HDMI** (`drivers/hdmi/hdmi.c`): 2 PIO SMs + 18 instructions on pio2.

```
ISR (core1) writes a line of 400 palette-index BYTES (= 800 output pixels),
   sync 240..243, Data-Island/audio 184..199 + 216..239, scanline, DS80 pairs,
   dither and the CRT grille all being ordinary indices
 -> dma_chan (8-bit)            -> PIO conv (8 instr): byte -> (page<<12)|(byte<<4)
 -> dma_chan_pal_conv_ctrl      -> that address into the next channel's read_addr
 -> dma_chan_pal_conv (4 words) -> PIO TMDS (10 instr) -> 6 data pins + side-set clock
```

`conv_color[256]` holds **2 x uint64** per index: each is 10 TMDS bits x 3 channels
already turned into 6-bit-per-cycle **differential** words by `get_ser_diff_data()`.
Cost: 16 B per index, 4 DMA words per index = **50.4 M transfers/s ~ 201 MB/s**.
PIO clock = the TMDS bit rate, 252 MHz, `pio_clk_div = sys/252` (1.0 / 1.5 / 2.0).

**VGA** (`drivers/vga-nextgen/vga.c`): 1 PIO SM, **1 instruction** (`out pins, 8`),
2 DMA channels. The ISR applies the palette **on the CPU**, one lookup per source
pixel, into `lines_pattern` line buffers of `uint16` pairs (2 output pixels per index,
`vga_pack_pair`). 1 byte per output pixel, ~800 B per line, **~20 MB/s**.

The DAC is a resistor ladder: 6 colour bits (`& 0x3f3f`, R2G2B2 = 64 hard colours) with
HS/VS in bits 6/7 (`palette16_mask = 0xc0c0`). `vga_bayer4` (`/21`) spreads each colour
over the 2x2 block -> **13 levels per channel, 13^3 = 2197 perceived colours**.

## HSTX facts (verified against pico-examples and the datasheet)

- Max **150 MHz** `clk_hstx`, DDR -> **300 Mbps per pin**.
- `hstx_ctrl_hw->bit[i]` for GPIO 12+i picks `SEL_P` (rising edge) and `SEL_N`
  (falling edge) out of the 32-bit shift register, plus `INV` and `CLK`.
- `CSR`: `SHIFT` bits shifted per cycle, `N_SHIFTS` shifts before popping the next
  FIFO word, `CLKDIV` = output-clock period in cycles, `EXPAND_EN` for the command
  expander (**not needed here** — we feed one word per output pixel).
- DMA writes `&hstx_fifo_hw->fifo` with `DREQ_HSTX`.
- `gpio_set_function(12..19, GPIO_FUNC_HSTX)`.

### HDMI word format

In variant B this raw form carries the CONTROL and Data-Island words (sync, porches,
preambles, guard bands, TERC4 packets); the active pixels go through the hardware
encoder as XRGB8888. Both share the same `bit[]` map and the same wire order.

`CSR: SHIFT=2, N_SHIFTS=5, CLKDIV=5`, `clk_hstx = 126 MHz` -> 252 Mbps/pin, 25.2 MHz
pixel — the same pixel clock the PIO path produces today, so the whole video-mode
table is unchanged. `126 = 252/2 = 378/3 = 504/4`: **an exact integer divider at every
CPU clock the Overclock menu offers**, where the PIO path needs a half-integer at 378.

Word = `ch0 | ch1<<10 | ch2<<20` with `bit[].SEL_P = lane*10`, `SEL_N = lane*10+1`.

**Bit order matches ours exactly.** `get_ser_diff_data()` puts symbol bit 9 at the top
of the accumulator and the PIO's `out pins,6` with `shift_right` emits the low bits
first, i.e. symbol bit 0 first; HSTX shifts the register right, also bit 0 first. The
four control symbols are the same constants (`0x354/0x0AB/0x154/0x2AB`).

**m2p2 lane map.** Today `d6 = (bR<<4)|(bG<<2)|bB` with `invert_diffpairs=1`, so
GP14/15 = ch0 (blue, carries sync), GP16/17 = ch1, GP18/19 = ch2, the **even** pin of
each pair inverted, clock on GP12/13. In HSTX that is `bit[2..7]` with `SEL_P/SEL_N` as
above and `INV` on the even pin, `bit[0] = CLK|INV`, `bit[1] = CLK`. Wire-identical to
what ships today — which is the point: the port cannot introduce a new pinout or
polarity bug, and that is provable offline (step 0 of the work plan).

### VGA word format — 4-phase PWM

From `DnCraptor/quakegeneric`, `drivers/dvi_hstx/` (author wbcbz7, **MIT**), which is
where the owner's "more colours on VGA" came from. Its README: *"VGA, 640x480 60Hz,
RGB222, with 2197-color high-speed PWM on HSTX-capable boards"*.

`CSR: SHIFT=16, N_SHIFTS=phase_repeats, CLKDIV=4`, `bit[pin] = SEL_P=k | SEL_N=k+8`.
Two different 8-bit words go out per `clk_hstx` cycle (one per edge), so a 32-bit FIFO
word is **4 PWM phases of one pixel**; each phase byte is the ordinary VGA byte
(b7 VS, b6 HS, b5-4 R, b3-2 G, b1-0 B). `vga_pwm_xlat_table[16]` maps a 4-bit channel
level to 4 two-bit phases whose sums cover 0..12 -> **13 levels per channel**, with the
pattern spread (`0,1,0,1` rather than `0,0,1,1`) to push the ripple up an octave.
`clk_hstx ~ 2 x pixel clock` (~50 MHz at 25.175) — far inside the 150 MHz ceiling.

**This is not "more bits", it is per-pixel PWM**, and that is exactly why it is worth
having here. The level count is the same 2197 we already reach with Bayer, but ours is
an average over a 2x2 block, which is why:

- 1-pixel detail (attribute cells, DS80/GMX/Timex 512-px modes, 1-px dither in demos)
  beats against the dither pattern, and
- the 16 flat ZX colours are deliberately forced **solid** (`vga_set_palette_entry_solid`
  + `vgaGridSnap`) to stop them shimmering — so they do *not* get the 13 levels, and a
  non-Pulsar palette stays biased dark (see the VGA colour-depth section in CLAUDE.md).

With PWM every pixel carries its own level, so the ZX palette and TS-Conf artwork are
both exact, and the whole solid / dither / grid-snap fork disappears.

## The design: one backend, both outputs, command expander

**Decision (owner, 2026-09-17): variant B — the HSTX command expander.** We are paying
for HSTX anyway, so take the version that also returns the palette slots. Variant A
(raw TMDS words on the existing index-byte stream) is kept only as the fallback in
"Rejected / considered" below.

### Why B is the one that frees palette slots

The 256-slot ceiling comes from the 8bpp framebuffer and cannot be raised by any
backend. But **72 of those 256 are not colours today**, and the reason is
architectural rather than electrical: sync, porches, preambles, guard bands and the
Data-Island packets all travel on the SAME index-byte stream as the pixels, so each
one has to BE a palette entry. See the palette section below for the full ledger.

To get non-pixel words into the stream without spending indices, the line has to be a
buffer of WORDS with a command list, which is exactly what the expander is for:

```
[HSTX_CMD_RAW_REPEAT | front_porch] [sync word]
[HSTX_CMD_RAW_REPEAT | hsync]       [sync word]
[HSTX_CMD_RAW_REPEAT | back_porch]  [sync word]        (+ island block when audio)
[HSTX_CMD_RAW_REPEAT | 8]           [video preamble]
[HSTX_CMD_RAW_REPEAT | 2]           [video guard]
[HSTX_CMD_TMDS | active]            -> the pixel words
```

and the consequence is that **the index -> word expansion moves to the CPU**. The PIO
converter and its DMA chain retire completely. That is how quakegeneric does it
(`drivers/dvi_hstx/linebuf_cb/index8.S`, a hand-written inner loop of
`ldrb` / `ldr [pal, idx lsl 2]` / `str`, three instructions per pixel).

### Shape

- **Line buffer = words.** Two ping-pong buffers, each a prebuilt template whose
  blanking command words are constant per line TYPE; the ISR only fills the active
  region (and, with audio, the island block). Blanking-only lines and the scanline
  line are whole static buffers, the way `lines_pattern[0/1]` and `hdmi_scanline_buf`
  already are.
- **Pixels are XRGB8888 through the hardware TMDS encoder**
  (`EXPAND_TMDS` L2/L1/L0 NBITS=7, ROT 16/8/0 — quakegeneric's `DVI_HSTX_MODE_XRGB8888`),
  so full 8 bits per channel and **hardware running disparity**.
- **`pix_rep` is unusable here.** The reference doubles pixels in the expander
  (`ENC_N_SHIFTS = pix_rep`, one word per source pixel); our two output pixels must be
  able to DIFFER — the CRT aperture grille and the DS80/GMX/Timex pair modes are built
  on exactly that. So `pix_rep = 1` and two words per source pixel. The lever if DMA or
  ISR ever bite: RGB565 with `ENC_N_SHIFTS=2, ENC_SHIFT=16` puts two different pixels in
  ONE word (back to 320 words per line, ~41 MB/s), at 5/6/5 — rejected for now because
  these palettes are tuned to the code unit.
- **The LUT becomes an ordinary array**: 256 x 8 bytes per grille page. No 4 KB
  alignment, no `.hdmi_lut` section, no SCRATCH_Y page-B placement — those exist only
  because the PIO address converter reconstructs `(page << 12) | (byte << 4)`.
- **VGA rides the same structure** (the owner's "do not split"): same word line buffer,
  same command list, `vga_sync_word[]` in place of the TMDS control symbols, and each
  LUT entry holding the 4-phase PWM words instead of colour. One `hstx_start(mode,
  is_vga)` branching like quakegeneric's `hstx_init()`.

### What it costs and buys, measured against today

| | today (PIO) | A (raw) | **B (expander)** |
|---|---|---|---|
| palette slots for colour | 184 | 184 | **239**, regardless of HDMI audio |
| DMA transfers per line | 2400 | 1600 | **~680** |
| DMA bytes per line | 6400 | 3200 | ~2700 |
| DMA channels | 4 | 4 | **2** |
| PIO | 2 SM, 18 instr | 1 SM, 8 instr | **none** |
| ISR, audio off | 9 us | 9 us | ~10.5 us |
| ISR, audio on | 18-19 us | ~18.5 us | **about unchanged** |
| running disparity | software | software | **hardware** |
| 90/75 Hz modes | yes | no | no |

Three results worth keeping:

- **B is cheaper on DMA than A**, by 3.5x rather than 2x: the byte-feeder channel, the
  converter ctrl channel and the per-index `read_addr` ping-pong all disappear. One
  line becomes one transfer.
- **The lookup loop costs ~+1.5 us per pass** (~+0.4 ms/frame on core1), not the 2-3x I
  first assumed: on M33 it is `ldrb` + `ldrd` (the 8-byte LUT entry) + `strd`, the same
  three instructions per pixel the reference uses. The `^2` source swizzle is handled
  the way the DS80 fast path already does it — read four source bytes as one word and
  rotate.
- **With audio it is roughly self-cancelling**: `hdmi_di_load` copies 2 pages x 32
  uint64 = 512 B per line today; in B it writes ~36 words = 144 B into the line buffer.
  Islands get 3.5x cheaper.

### Hardware disparity retires a whole layer

`tmds_pair.h`, the balanced-pair construction, `HDMI_TMDS_LEVEL_CLAMP` and the
capture-safe single-symbol snapping (`Config::hdmi_snap`) all exist because we encode
TMDS in software with no running-disparity state. The encoder in HSTX keeps it in
hardware, so those go away — and the capture-card artifact they were fighting (a solid
colour returning as two alternating colours) goes with them: it was caused by our pair
carrying v and v+-1, two different VALUES. Hardware TMDS sends two symbols that both
decode to exactly v.

## Palette slots: the ledger, and what is free without HSTX

The motivation for B, and the one number a user sees. `ts256PoolInit()` (Video.cpp)
skips `152..167`, `184..199` and everything from `216`:

| range | slots | actually needed when |
|---|---|---|
| 152..167 UI palette | 16 | menu and `OSD::notify` — always |
| 184..199 DI set 1 | 16 | HDMI **and** HDMI audio |
| 216..239 preambles, guards, DI set 0 | 24 | HDMI **and** HDMI audio |
| 240..243 sync | 4 | HDMI only (VGA blanks through `bg_color[]`) |
| 244 scanline | 1 | only with scanlines on |
| **245..254** | **10** | **nothing at all** |
| 255 border | 1 | HDMI |

72 reserved, pool 184; only the 16 UI slots are structurally unavoidable.

**Available today, with no HSTX at all** (worth doing as its own small commit — the
owner has deferred it, not declined it):

1. **+10**: `245..254` are caught by a blanket `i >= BASE_HDMI_CTRL_INX` in
   `hdmi_palette_slot_writable()` and `if (i >= 216) continue;` in the pool. Nothing
   uses them. Narrow both to `240..244`, initialise the ten to black at init (an
   unprogrammed slot would emit a zero word).
2. **+40** when HDMI audio is off or the output is VGA. The neighbouring code already
   knows how: `init_profi_pair_lookup()` computes
   `reserve_di = !SELECT_VGA && Config::audio_driver == 4`. `ts256PoolInit()` is static
   and reserves the DI ranges even in a VGA session, where Data Islands do not exist.
   Needs the same predicate plus a pool rebuild on the audio-driver edge
   (`applyPalette()` already re-flushes the whole map).
3. **+6 on VGA**: sync, scanline and border go through `bg_color[]` / prebuilt sync
   lines, not palette indices.

| configuration | now | after the free fixes | after B |
|---|---|---|---|
| HDMI + HDMI audio | 184 | 194 | **239** |
| HDMI + I2S/PWM/Covox | 184 | 234 | **239** |
| VGA | 184 | 240 | **239** |

Second beneficiary of B: `init_profi_pair_lookup()` drops from 250 usable pairs to 210
with HDMI audio and compensates with 40 extra bright-ink x bright-paper merges — so
DS80/GMX/Timex colours are measurably less faithful with HDMI sound than without. B
removes that trade too.

Second-order: `ts256PickBanks()` splits the pool into 2-4 palette-version banks. At 184
slots four banks are 46 each; at 239 they are 59. More slots means fewer nearest-colour
merges AND versioning available to more titles (the RobFgift case).

## The other two findings from the design discussion

**PIO fractional-divider jitter at 378 MHz.** `PIO_DIV = CPU_MHZ / 252`, so the default
clock runs the TMDS SM at divider **1.5** — and a fractional PIO divider is a
clock-enable counter, so the state machine advances at intervals of 1, 2, 1, 2 system
cycles. At 378 MHz (2.6455 ns) the bit boundaries inside a character land at cycles
0,1,3,4,6,7,9,10,12,13 of 15. A receiver sampling on the uniform grid recovered from
the (exact) 25.2 MHz character clock still hits every bit, but **the margin on the
narrow bits is 0.25 cycle = 0.66 ns instead of 1.98 ns — the data eye is squeezed
threefold**, before rise time and cable. 252 (div 1.0) and 504 (div 2.0) are clean.
Consequence: the comment in `graphics.c` — *"must be integer or half-integer (n/2) for
clean TMDS pixel clock"* — is wrong about the half-integer case. HSTX removes this
(`clk_hstx` = 126 MHz, an integer divide of all three CPU clocks, and no clock-enable
counter). **Checkable today without any code**: a marginal sink at 378 versus 252/504.

**CPU offload is a VGA story, not an HDMI one.** Neither PIO nor HSTX spends CPU on
pixels, so on HDMI the direct saving is zero (B's numbers above are the lookup loop
against the island copies). On VGA it is real and large: `vga.c` applies the palette on
the CPU *and* runs the full conversion on **every output line** (480 per frame), not
once per pair like HDMI (240), because `palette_vga16[screen_line & 1]` is the Bayer row
phase. Today's 2197 VGA colours are paid for by doubling the ISR. With 4-phase PWM the
row phase disappears, the pair renders once, and the estimate is ~2.5 ms/frame ->
~0.7 ms/frame of core1. That is an estimate from instruction counts: **VGA has no ISR
duration counter at all** (HDMI has `hdmi_irq_max_gap_us`/`hdmi_irq_max_dur_us`) —
adding the twin is ten lines and gives both the before and the after.

## Work plan

Land as one feature on one branch; the numbered steps are an order of work, not
separate releases.

0. **Host tests.** The only part that can be verified in a container without the SDK.
   `tools/hstx_word_test.c`: for all 256 palette values, that the XRGB8888 LUT entry
   fed through the documented `EXPAND_TMDS` field extraction yields the colour we
   intended, and that the command words (`RAW_REPEAT | n`, `TMDS | n`) and the four
   control symbols are laid out as the expander reads them. On the VGA side: level ->
   4 phases -> mean code value against the intended RGB888, and the 13-level ladder.
   Precedent: `tools/hdmi_tmds_pair_test.c`. Re-run after any change to the packing.
1. **Line model.** Word line buffers, the per-line-type command templates, and the
   index -> word inner loop (`ldrb` / `ldrd` / `strd`, four source bytes read as one
   word for the `^2` swizzle, two grille pages alternating by pixel parity). The ISR
   keeps its structure — what changes is where it writes and what a "palette entry" is.
2. **`drivers/hstx/`** — per-board lane table, `hstx_start(mode, is_vga)` branching like
   quakegeneric's `hstx_init()`, `clk_hstx`, expander configuration, pad drive. Retire
   the PIO converter, the byte-feeder and the converter ctrl channel; two DMA channels
   remain. `hdmi_audio_hw_init()` derives the pixel clock from `clock_get_hz(clk_hstx)`.
3. **Structural words leave the palette**: sync/porch/preamble/guard into the line
   templates, `hdmi_di_load` writing island words into the line buffer instead of LUT
   slots, and `ts256PoolInit()` / `hdmi_palette_slot_writable()` / `init_profi_pair_lookup()`
   widened to the freed range. Retire `tmds_pair.h`, the balanced pair, LEVEL_CLAMP and
   `hdmi_snap` (hardware disparity). VGA side: PWM LUT, sync words, and the
   render-once-per-pair change the disappearing Bayer row phase allows.
4. **Build** — five lines, no matrix change:
   ```cmake
   option(HDMI_HSTX "..." OFF)            # engineering A/B only, not in the matrix
   IF(MURM2)
       if(NOT TFT AND NOT TV AND NOT SOFTTV)
           set(HDMI_HSTX ON)              # their `if (VGA_HDMI) set(DVI_HSTX ON)`
       endif()
   ...
   if(HDMI_HSTX)
       target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_HSTX=1)
       SET(BUILD_NAME "${BUILD_NAME}-HSTX")
       SET(DISPLAY_TAG "${DISPLAY_TAG}HSTX")
   endif()
   ```
   `build_all.sh` untouched. `check-release.sh` needs its m2p2 glob updated — note
   that its `BOARDS` table still matches `m2-speccy-...`/`PC-speccy-...` while the real
   tags have been `m2p2`/`PCp2` for a long time, so that SRAM-headroom check currently
   matches nothing on any board. Separate one-line fix.
   `.vscode/tasks.json` is gitignored — the F7 picker entry is added by hand.
5. **Hardware** (only the owner can): picture and sync on m2p2 at 252/378/504; the
   same image on the VGA jumper (PWM colour, ZX palette solid-looking without
   `vgaGridSnap`, TS-Conf 256c artwork); HDMI audio (`HDMIAU: dur/gap/skip/dup/und`);
   menu, F8 stats, FDD lamp, notify banner; DS80/GMX/Timex pair modes; scanlines; CRT
   grille; a capture card; and `[PERF] 60f` before/after, which is the reason for all
   of this.
6. **Optional afterwards**: LUT slot stride 16 -> 8 bytes (`in x,20` -> `in x,21` in the
   converter, page base 2 KB-aligned) returns ~2 KB of SRAM to both outputs.

## Open hardware questions

- Behaviour of HSTX on an **empty FIFO** (the PIO stalls its SM and the clock with it,
  which receivers survive). Check the datasheet before trusting the first capture.
- Clock phase/polarity on GP12/13 (`CLKPHASE`); if the picture is noisy this is the
  first one-line A/B.
- RP2350 errata touching HSTX.
- Whether the resistor ladder plus the monitor's input integrate 4 phases cleanly at
  ~10 ns steps. It works on their hardware; ours is the same class of board, but this
  is the one part that cannot be reasoned about.
- This container has no Pico SDK, so nothing but step 0 can be compiled here.

## Rejected / already considered, do not re-derive

- **Separate `-HSTX` firmware or a display target beside `VGA_HDMI`** — the owner wants
  one image, as in the original.
- **Landing HDMI and VGA as two phases** — same instruction: not split.
- **Variant A — raw TMDS words on the existing index-byte stream.** Keeps the ISR and
  the PIO converter untouched and is provable offline pin for pin, but it frees no
  palette slots (the structural words still have to be palette entries) and costs more
  DMA than B. Kept as the fallback if B's lookup loop turns out to hurt a core1-bound
  title more than the measurement suggests; the word layer is shared, so A is a
  retreat, not a rewrite.
- **Commands inside the index stream** (structural indices whose 2-word LUT entry holds
  `[CMD_RAW_REPEAT | n][sync]`, with the guard band arranged so the `CMD_TMDS` word
  lands on a slot boundary). It works for blanking, but the island's 32 packet words
  still need LUT entries — so it frees almost nothing and buys the complexity for free.
- **`pix_rep` in the expander** — see the design section: our two output pixels must be
  able to differ.
- **Moving VGA to HSTX "for more bits"** — the ladder is 2 bits per channel and HSTX
  drives the same 8 pins; the gain is PWM, not depth. HSTX is also strictly less
  flexible for VGA (8 fixed pins, so no wider ladder is ever possible on it).
- **Sub-pixel PWM on PIO instead** — possible in principle (`out pins,8`, one
  sub-phase per cycle up to sys_clk) and it would need no HSTX at all, but without the
  expander it costs one FIFO byte per phase, i.e. several times the DMA of the HSTX
  form. Not a reason to skip HSTX.

## Sources

- `raspberrypi/pico-examples`, `hstx/dvi_out_hstx_encoder/dvi_out_hstx_encoder.c` —
  register semantics for the DVI case (BSD-3-Clause).
- `DnCraptor/quakegeneric`, `drivers/dvi_hstx/` and `drivers/dvi_hstx_hdmi_audio/` —
  wbcbz7, **MIT**; `data_packet.c` is Shuichi Takano -> mlorenzati/PicoDVI, also MIT.
  The VGA 4-phase PWM and a working HSTX HDMI-audio path (TERC4 data islands, AVI /
  ACR / Audio InfoFrame) both live there. MIT is GPL-3 compatible: this code may be
  reused with its copyright notices kept.
- RP2350 datasheet, HSTX chapter (150 MHz, 300 Mbps/pin DDR).
