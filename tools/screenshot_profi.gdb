set pagination off
# Dump Profi DS80 framebuffer and decoding tables.
# Requires profi_ds80_active == 1 for meaningful content,
# but dumps unconditionally so you can run it any time.
printf "profi_ds80_active: %d\n", profi_ds80_active

set $w = VIDEO::vga.xres
set $h = VIDEO::vga.yres
set $fb0 = VIDEO::vga.frameBuffer[0]
printf "screenshot profi: %dx%d fb=%p\n", $w, $h, $fb0

# Framebuffer: w*h bytes, row-major (320*240 = 76800 B)
# Layout per row: 32-byte black pad | 256-byte content (packed pairs) | 32-byte pad
# The main framebuffer is a ROW-POINTER table (VIDEO::vga.frameBuffer[y]) and
# since 2026-09-10 may be 2..8 whole-row chunks scattered over the heap, so a
# single dump of fb0..fb0+w*h reads garbage past the first chunk (hw 2026-09-14:
# a 720x576 capture came out as the top rows + noise). Dump one row at a time.
set $y = 0
while $y < $h
  if $y == 0
    dump binary memory /tmp/picospec_profi_fb.bin VIDEO::vga.frameBuffer[0] (VIDEO::vga.frameBuffer[0] + $w)
  else
    append binary memory /tmp/picospec_profi_fb.bin VIDEO::vga.frameBuffer[$y] (VIDEO::vga.frameBuffer[$y] + $w)
  end
  set $y = $y + 1
end

# Pair lookup table: uint8_t[16][16], row-major [ink][paper] → DS80 slot index
dump binary memory /tmp/picospec_profi_lut.bin \
    &VIDEO::profi_pair_lookup \
    ((char*)(&VIDEO::profi_pair_lookup) + 256)

# Live palette: uint32_t[16], little-endian 0x00RRGGBB
dump binary memory /tmp/picospec_profi_pal.bin \
    &VIDEO::profi_palette_live \
    ((char*)(&VIDEO::profi_palette_live) + 64)

# Write framebuffer dimensions
set logging file /tmp/picospec_profi_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", $w, $h
set logging enabled off
printf "screenshot profi: dump done\n"
