#include "video.h"
#include <spiram.h>
#include <uzebox.h>

/*
 * displayFrameReady is the ownership handshake for the one AVR framebuffer:
 *
 *   0: foreground/game code owns displayBuffer and may draw into it.
 *   1: displayBuffer contains a completed immutable frame. The video-mode
 *      renderer owns it until the complete 160-scanline active region has been emitted.
 *
 * The assembly renderer latches this byte once when it takes over the frame.
 * If set, it scans directly from SRAM without touching SPI. At frame end it
 * clears this byte; foreground then copies the immutable 160x80 framebuffer to
 * the SPI repeat buffer during post-active blanking. The renderer owns only the
 * 160 framebuffer scanlines and then returns to foreground execution.
 */
volatile uint8_t displayFrameReady;
volatile uint8_t displayActiveEpoch;
volatile uint8_t displayVsyncActiveEpoch;

/* A published SRAM frame is first displayed directly by the no-SPI renderer.
 * After that complete active frame retires, foreground copies the same immutable
 * bytes to the SPI repeat buffer before releasing SRAM back to game drawing. */
static volatile uint8_t displayBackingDirty;

void initializeDisplay(void) {
    displayFrameReady = 0;
    displayActiveEpoch = 0;
    displayVsyncActiveEpoch = 0;
    displayBackingDirty = 0;
}

void updateDisplay(void) {
    /* Publishing freezes SRAM until two ordered operations finish: first a
     * complete direct-SRAM video frame, then a foreground backing copy to SPI. */
    displayBackingDirty = 1;
    displayFrameReady = 1;
}

void waitDisplayBufferAvailable(void) {
    /* Interrupts stay enabled while the no-SPI renderer consumes a published
     * SRAM frame. It clears displayFrameReady only after all 160 active
     * scanlines have retired. At that instant we are at the start of the large
     * post-active blanking window, so copy the still-immutable SRAM frame to the
     * SPI repeat buffer now. The 2bpp mode copies 3200 bytes; it remains a
     * single sequential transaction during the post-active blanking window. */
    while (displayFrameReady) { }
    if (displayBackingDirty) {
        SpiRamWriteFrom(DISPLAY_BUFFER_BANK, DISPLAY_BUFFER_ADDR,
                        displayBuffer, DISPLAY_BYTES);
        displayBackingDirty = 0;
    }
}

void displayWaitNextActiveEnd(void) {
    /* VideoModeVsync snapshots the preceding active-end epoch at the exact VSYNC
     * boundary. Wait only until this field's active region retires. If lengthy
     * foreground work has already crossed that point, return immediately. */
    uint8_t e=displayVsyncActiveEpoch;
    while(displayActiveEpoch==e) { }
}

void displayVsyncTick(void) {
    displayVsyncActiveEpoch=displayActiveEpoch;
}
