#pragma once

#include QUOTE(VIDEO_MODE_PATH/videoModeConfig.h)

#define VMODE_ASM_SOURCE       QUOTE(VIDEO_MODE_PATH/videoModeCore.s)
#define VMODE_C_SOURCE         QUOTE(VIDEO_MODE_PATH/videoMode.c)
#define VMODE_C_PROTOTYPES     QUOTE(VIDEO_MODE_PATH/videoMode.h)
#define VMODE_FUNC             sub_video_mode

/* HUD-less Fear Campaign output: 160 active bitmap scanlines. The custom
 * full-frame core consumes one kernel line establishing its first manual HSYNC
 * epoch before emitting bitmap line 0. Enter at line 51 so that alignment line
 * stays in blanking and the first real bitmap scanline begins at line 52, which
 * is the intended centered 160-line window. */
#define FIRST_RENDER_LINE 51
#define FRAME_LINES DISPLAY_SCANLINES

#define VRAM_SIZE      0
#define VRAM_ADDR_SIZE 1
#define VRAM_PTR_TYPE  unsigned char

/* Fear Campaign uses a four-channel Missile Command POKEY subset. The custom
 * inline path is 216 clocks, below the kernel's 230-cycle video-entry target. */
#undef AUDIO_OUT_HSYNC_CYCLES
#undef AUDIO_OUT_VSYNC_CYCLES
#define AUDIO_OUT_HSYNC_CYCLES 216
#define AUDIO_OUT_VSYNC_CYCLES 216
#define HSYNC_USABLE_CYCLES 230

#if FRAME_LINES != DISPLAY_SCANLINES
#error FRAME_LINES must equal the 160 framebuffer scanlines.
#endif
