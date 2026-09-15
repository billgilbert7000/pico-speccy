#include "graphics.h"
#include <string.h>
#include "pico.h"

uint16_t graphics_max_tft_freq_mhz = 126;

// PIO clock divider must be integer or half-integer (n/2) for clean TMDS pixel clock.
// TMDS bit clock = pixel_clock * 10; the HDMI PIO program is 10 instructions
// (one bit per lane per cycle, clock pair as side-set), so SM clock = TMDS rate.
//
// TMDS_STD_MHZ 252 = 25.2 MHz pixel, and it is the same at EVERY CPU clock:
// 252/1.0, 378/1.5, 504/2.0 all land on it, which is why the timing table does
// not depend on the Overclock setting.  h_total is 800 px (line_bytes 400 x 2)
// in every standard mode → 31.5 kHz line rate, 31.75 µs per line.
//
// TMDS_FAST_MHZ 378 = 37.8 MHz pixel: the same tables at x1.5 the pixel rate,
// i.e. 47.25 kHz / 21.16 µs per line and x1.5 the refresh (90 / 75 Hz).  Only
// sys_clk 378 gives it a clean divider (1.0), so those modes are gated on
// Config::cpu_mhz == 378 — see Config::isFastVideoMode().
#define TMDS_STD_MHZ   252
#define TMDS_FAST_MHZ  378
#define PIO_DIV        ((float)CPU_MHZ / (float)TMDS_STD_MHZ)
// Clamped at >= 1.0: a build whose boot clock is below 378 MHz (ZERO2) cannot
// reach this TMDS rate at all, and sm_config_set_clkdiv() asserts on a divider
// under 1. Such a board can still select these modes after raising the CPU clock
// in the menu — graphics_set_sys_clk_mhz() then recomputes the whole table.
#define PIO_DIV_FAST   (CPU_MHZ < TMDS_FAST_MHZ ? 1.0f : (float)CPU_MHZ / (float)TMDS_FAST_MHZ)

static struct video_mode_t video_mode[] = {
    { // [0] 640x480 60Hz
        .v_total = 524,
        .v_active = 480,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV
    },
    { // [1] 640x480 50Hz Pentagon 48.82Hz
        .v_total = 644,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=19.89MHz (div=19@378MHz), v_total=511 → Pentagon-like
        .vga_v_total = 511,
        .vga_pixel_clk = 19894737
    },
    { // [2] 640x480 50Hz 48K 50.08Hz
        .v_total = 628,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=19.89MHz (div=19@378MHz)
        .vga_v_total = 499,
        .vga_pixel_clk = 19894737
    },
    { // [3] 640x480 50Hz 128K 50.02Hz
        .v_total = 629,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=19.89MHz (div=19@378MHz)
        .vga_v_total = 498,
        .vga_pixel_clk = 19894737
    },
    { // [4] 720x576 50Hz Pentagon full border — 25.2MHz pixel (sys_clk=378MHz, div=1.5)
        .v_total = 644,   // 25.2MHz/800/644 = 48.91Hz (Pentagon 48.83Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=27MHz (div=14@378), line_size=864 (HS=48, BP=96, active=720, FP=0)
        // → active/h_total = 720/864 = 83.3% PAL aspect
        // v_total empirically tuned to 628 for 48.83Hz Pentagon on actual HW
        .vga_v_total = 628,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 0,
        .vga_screen_width = 360
    },
    { // [5] 720x576 50Hz 48K full border — 25.2MHz pixel
        .v_total = 628,   // 25.2MHz/800/628 = 50.09Hz (48K 50.08Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=27MHz (div=14@378), line_size=864 → 720/864=83.3% PAL aspect
        // v_total empirically tuned to 613 for 50.08Hz 48K on actual HW
        .vga_v_total = 614,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 0,
        .vga_screen_width = 360
    },
    { // [6] 720x576 50Hz 128K full border — 25.2MHz pixel
        .v_total = 629,   // 25.2MHz/800/629 = 50.00Hz (128K 50.02Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=27MHz (div=14@378), line_size=864 → 720/864=83.3% PAL aspect
        // v_total empirically tuned to 613 for 50.02Hz 128K on actual HW
        .vga_v_total = 612,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 0,
        .vga_screen_width = 360
    },
    { // [7] 720x480 60Hz half border
        .v_total = 524,
        .v_active = 480,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: pixel_clk=27MHz (div=14@378), line_size=864 (HS=48, BP=96, active=720, FP=0)
        // → 720/864 = 83.3% PAL aspect, h_freq=31.25kHz, v_total=521 → 60.00Hz
        .vga_v_total = 521,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 500,
        .vga_vsync_end = 502,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 0,
        .vga_screen_width = 360
    },
    { // [8] 720x576 60Hz full border — 25.2MHz pixel (non-standard: v_active>v_total)
        .v_total = 524,   // 25.2MHz/800/524 ≈ 60.1Hz; v_active=576>524 so all lines are active
        .v_active = 576,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV
    },

    // ---------------------------------------------------------------------
    // 37.8 MHz pixel clock (sys_clk 378 MHz, PIO divider 1.0 — no fractional
    // divider at all).  Byte-for-byte the tables above: same h_total (800 px),
    // same v_total, so the line rate is 47.25 kHz and every refresh is exactly
    // x1.5.  Each sits at its standard twin's index + VMODE_FAST_OFFSET, which
    // is what graphics_fast_mode() resolves for VIDEO::Reset().
    //
    // NOTE the refresh is the DISPLAY's, not the machine's: with V-Sync on the
    // emulated frame rate follows it, so these modes force V-Sync off (see
    // resolveConstraints in UiStage.cpp).
    // ---------------------------------------------------------------------
    { // [9] 640x480 90Hz — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/524 = 90.17Hz (x1.5 of mode [0])
        .v_total = 524,
        .v_active = 480,
        .freq = 90,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [10] 640x480 75Hz Pentagon — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/644 = 73.37Hz (x1.5 of mode [1], Pentagon 48.83 x 1.5)
        .v_total = 644,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [11] 640x480 75Hz 48K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/628 = 75.24Hz (x1.5 of mode [2])
        .v_total = 628,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [12] 640x480 75Hz 128K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/629 = 75.12Hz (x1.5 of mode [3])
        .v_total = 629,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [13] 720x576 75Hz Pentagon — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/644 = 73.37Hz (x1.5 of mode [4])
        .v_total = 644,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [14] 720x576 75Hz 48K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/628 = 75.24Hz (x1.5 of mode [5])
        .v_total = 628,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [15] 720x576 75Hz 128K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/629 = 75.12Hz (x1.5 of mode [6])
        .v_total = 629,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [16] 720x480 90Hz — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/524 = 90.17Hz (x1.5 of mode [7]), half border
        .v_total = 524,
        .v_active = 480,
        .freq = 90,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    }
};

/**
void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
if (!text_buffer) return;
    uint8_t* t_buf = text_buffer + TEXTMODE_COLS * 2 * y + 2 * x;
    for (int xi = TEXTMODE_COLS * 2; xi--;) {
        if (!*string) break;
        *t_buf++ = *string++;
        *t_buf++ = bgcolor << 4 | color & 0xF;
    }
}
*/
void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    char line[width + 1];
    memset(line, 0, sizeof line);
    width--;
    height--;
    // Рисуем рамки

    memset(line, 0xCD, width); // ═══


    line[0] = 0xC9; // ╔
    line[width] = 0xBB; // ╗
    draw_text(line, x, y, 11, 1);

    line[0] = 0xC8; // ╚
    line[width] = 0xBC; //  ╝
    draw_text(line, x, height + y, 11, 1);

    memset(line, ' ', width);
    line[0] = line[width] = 0xBA;

    for (int i = 1; i < height; i++) {
        draw_text(line, x, y + i, 11, 1);
    }

    snprintf(line, width - 1, " %s ", title);
    draw_text(line, x + (width - strlen(line)) / 2, y, 14, 3);
}

struct video_mode_t __not_in_flash_func(graphics_get_video_mode)(int mode)
{
    return video_mode[mode];
}

int graphics_fast_mode(int mode)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    const int fast = mode + VMODE_FAST_OFFSET;
    if (mode < 0 || fast >= n) return mode;
    // Only answer for a real twin: index 8 (720x576@60) has none, and a caller
    // must never be handed an unrelated mode because the table grew.
    return video_mode[fast].tmds_mhz == TMDS_FAST_MHZ ? fast : mode;
}

float graphics_clk_div_at(int mode, unsigned sys_mhz, int vga)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    if (mode < 0 || mode >= n || !sys_mhz) return 0.0f;
    if (vga) {
        // vga_reinit(): clk_sys / pixel_clk, written to CLKDIV as 16.8 with the low
        // 12 bits masked off — i.e. the programmed divider is quantised to 1/16.
        const int pix = video_mode[mode].vga_pixel_clk ? video_mode[mode].vga_pixel_clk
                                                       : video_mode[mode].pixel_clk;
        if (pix <= 0) return 0.0f;
        const double fdiv = (double)sys_mhz * 1000000.0 / (double)pix;
        const uint32_t d32 = (uint32_t)(fdiv * 65536.0) & 0xfffff000u;
        return (float)d32 / 65536.0f;
    }
    // HDMI: the same formula graphics_set_sys_clk_mhz() stores, clamp and all —
    // except that "below 1" is reported as unreachable instead of clamped to 1.0,
    // which would claim a mode runs when it would silently run at the wrong rate.
    const int tmds = video_mode[mode].tmds_mhz ? video_mode[mode].tmds_mhz : TMDS_STD_MHZ;
    if ((float)sys_mhz < (float)tmds) return 0.0f;
    return (float)sys_mhz / (float)tmds;
}

void graphics_set_sys_clk_mhz(unsigned mhz)
{
    if (!mhz) return;
    for (int i = 0; i < sizeof(video_mode)/sizeof(video_mode[0]); i++) {
        const int tmds = video_mode[i].tmds_mhz ? video_mode[i].tmds_mhz : TMDS_STD_MHZ;
        float div = (float)mhz / (float)tmds;
        // A PIO divider below 1 does not exist; a mode asking for more TMDS than
        // sys_clk can give is simply unreachable at this clock and is gated off in
        // the menu (Config::isFastVideoMode).  Clamp so a stale pick cannot hand
        // pio_sm_init() an illegal divider.
        if (div < 1.0f) div = 1.0f;
        video_mode[i].pio_clk_div = div;
    }
}

#ifdef VGA_HDMI
extern void hdmi_set_scanlines(uint8_t level);
extern void vga_set_scanlines(uint8_t level);
void graphics_set_scanlines(uint8_t level) {
    hdmi_set_scanlines(level);
    vga_set_scanlines(level);
}
extern void hdmi_set_crt(uint8_t level);
extern void vga_set_crt(uint8_t level);
void graphics_set_crt(uint8_t level) {
    // Each backend rebuilds only its own tables from its own colour cache, so the
    // order does not matter and neither can clobber the other's state.
    hdmi_set_crt(level);
    vga_set_crt(level);
}
extern void hdmi_set_dither(bool enabled);
void graphics_set_dither(bool enabled) {
    hdmi_set_dither(enabled);
}
extern void hdmi_set_clock_drive(bool soft);
void graphics_set_hdmi_clock_drive(bool soft) {
    hdmi_set_clock_drive(soft);
}
// Only the HDMI backend caches the mode: its line ISR reads a snapshot taken in
// hdmi_init(). The VGA ISR fetches graphics_get_video_mode(get_video_mode())
// every line, and the 50 Hz variants of one resolution differ only in
// (vga_)v_total, so VGA already follows a machine switch live.
void graphics_update_mode_timing(void) {
    hdmi_update_mode_timing();
}
#else
void graphics_set_scanlines(uint8_t level) {
    (void)level;
}
// TFT/ILI9341, TV and SOFTTV targets do not link vga.c/hdmi.c — the grille has no
// output pair to use there, so it degrades to the colour half of the filter only.
void graphics_set_crt(uint8_t level) {
    (void)level;
}
#ifdef HDMI
extern void hdmi_set_dither(bool enabled);
void graphics_set_dither(bool enabled) {
    hdmi_set_dither(enabled);
}
extern void hdmi_set_clock_drive(bool soft);
void graphics_set_hdmi_clock_drive(bool soft) {
    hdmi_set_clock_drive(soft);
}
void graphics_update_mode_timing(void) {
    hdmi_update_mode_timing();
}
#else
void graphics_set_dither(bool enabled) {
    (void)enabled;
}
void graphics_set_hdmi_clock_drive(bool soft) {
    (void)soft;
}
// No cached mode timing on TV/SOFTTV/TFT: those drivers have their own,
// output-fixed frame rate.
void graphics_update_mode_timing(void) {
}
#endif
#endif