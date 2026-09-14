set pagination off
set $w = VIDEO::vga.xres
set $h = VIDEO::vga.yres
set $fb0 = VIDEO::vga.frameBuffer[0]
printf "screenshot: %dx%d fb=%p\n", $w, $h, $fb0
# The framebuffer is NOT one block any more (2026-09-10): a thin or fragmented heap
# splits it into 2-8 whole-row chunks, and FB_FORCE_CHUNKS builds always do. A
# dump of fb0..fb0+w*h then holds garbage past the first chunk (hw 2026-09-14:
# a 720x576 capture came out as one clean band over noise). Walk the row pointer
# table instead -- every driver reads rows through it, so this is the picture.
# `dump` creates the file, `append` extends it; each row is exactly $w bytes, so
# the result is the same contiguous w*h image fb2png.py has always expected.
dump binary memory /tmp/picospec_fb.bin $fb0 ($fb0 + $w)
set $y = 1
while $y < $h
  set $row = VIDEO::vga.frameBuffer[$y]
  append binary memory /tmp/picospec_fb.bin $row ($row + $w)
  set $y = $y + 1
end
# Dump 1KB at $fb0-aligned dummy zone — NOT a real palette. fb2png.py will
# auto-detect that the palette doesn't look right and fall back to the
# standard ZX palette, which is what we want for HDMI build anyway (real
# palette is optimized away into TMDS conv_color).
dump binary memory /tmp/picospec_pal.bin $fb0 ($fb0 + 1024)
printf "screenshot: palette placeholder dumped (ZX fallback will be used)\n"
set logging file /tmp/picospec_dim.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "%d %d\n", $w, $h
set logging enabled off
printf "screenshot: dump done\n"
