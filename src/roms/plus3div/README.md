# +3 (divIDE) ROM — not shipped, drop it here

This romset is Garry Lancaster's +3e / IDEDOS ROM built for a **divIDE** card (the
`div` build of `p3eroms`) rather than for the +3e's own "simple 8-bit" interface. It is
what reads the **16-bit** IDEDOS disks — Workbench and friends — because divIDE puts a
whole 512-byte sector through one data port (see `src/DivideIde.h`).

The image is not redistributable, so it is not in this repository and neither are the
files generated from it (`.gitignore` here keeps them out). Everything else — the port
decode, the `IDE::DIVIDE` scheme, the menu rows, the constraints — is in the tree and
inert until the ROM is packed.

## Adding it

1. Get the four 16 KB banks of the `div` build (`diven3e0..3` / `dives3e0..3`, or the
   `dives3e0..3.rom` set the Workbench download ships for Fuse) and save them here in
   bank order:

       src/roms/plus3div/src/rom0.bin   # editor / menu
       src/roms/plus3div/src/rom1.bin   # syntax checker
       src/roms/plus3div/src/rom2.bin   # +3DOS + IDEDOS  <- the divIDE driver lives here
       src/roms/plus3div/src/rom3.bin   # 48 BASIC

2. Pack them:

       python3 tools/rom_pack.py plus3div

   The packer measures every bank against the +3 / +3e banks already in flash and
   writes `plus3div_roms.c` + `plus3div_roms.h` (plus `manifest.json` and the `.ovl`
   artifacts). Expect roughly 14 KB of flash: the `div` build differs from the shipped
   `sm8` one in banks 1 and 2 only.

3. Rebuild. CMake turns `PLUS3DIV_IN_FLASH` on from the presence of
   `plus3div_roms.h`, and the romset appears as **Machine → 128K → +3 (divIDE)**.

Picking it selects the `DivIDE` IDE scheme by itself; mount a **full-sector** `.hdf`
(HDF flags bit 0 clear) in Storage → IDE/HDD. A half-sector image is the +3e's, and the
two are not interchangeable.

## Checking it

    g++ -O2 -Wall -Wextra -Isrc -o /tmp/divide tools/divide_ide_test.cpp && /tmp/divide

With `rom2.bin` in place that test also scans the driver's port setups and reports how
many of the eight ATA registers it reaches — eight means the decode and the ROM agree.
