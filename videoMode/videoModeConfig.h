#ifndef FEARCAMPAIGN_VIDEO_MODE_CONFIG_H
#define FEARCAMPAIGN_VIDEO_MODE_CONFIG_H

/*
 * Fear Campaign 2bpp framebuffer.
 * 160 x 80 indexed pixels, four pixels per byte = 3200 bytes.
 * Each logical row is emitted twice, producing 160 bitmap scanlines.
 */
#define DISPLAY_BUFFER_BANK   0
#define DISPLAY_BUFFER_ADDR   0x0000
#define DISPLAY_WIDTH         160
#define DISPLAY_HEIGHT        80
#define DISPLAY_STRIDE        (DISPLAY_WIDTH / 4)
#define DISPLAY_BYTES         (DISPLAY_STRIDE * DISPLAY_HEIGHT)
#define DISPLAY_SCANLINES     (DISPLAY_HEIGHT * 2)

/*
 * The 2bpp core emits one logical pixel every eight AVR clocks, so the
 * 160-pixel active span occupies 1280 clocks. The remaining line time is used
 * for horizontal centering and the fixed 1820-cycle scanline cadence.
 */
#define DISPLAY_HCENTER_CYCLES 48


#endif
