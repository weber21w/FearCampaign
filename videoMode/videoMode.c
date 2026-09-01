#include <avr/io.h>
#include <spiram.h>
#include <uzebox.h>
#include "videoMode.h"
#include "video.h"

/*
 * Called by the kernel before main(). The scanline renderer accesses SPI RAM
 * directly, so configure the same max-speed SPI settings used by spiram.s.
 * We intentionally do not run SpiRamInit()'s destructive presence/size test:
 * This video mode requires SPI RAM, and malformed/missing hardware is not a case
 * the game needs to spend flash handling at runtime. The SD card shares this
 * SPI bus on standard Uzebox hardware, so its CS is explicitly driven high.
 */
void InitializeVideoMode(void) {
    /* Custom mixer output is unsigned PCM; 0x80 is silence. */
    OCR2A = 0x80;

    SPCR = (1 << SPE) | (1 << MSTR);
    SPSR = (1 << SPI2X);
    DDRB |= (1 << PB7) | (1 << PB5);  /* SCK and MOSI outputs */

    /* Both devices share SPI. Keep the SD card explicitly deselected. */
    PORTD |= (1 << PD6);              /* SD card CS high */
    DDRD |= (1 << PD6);               /* SD card CS output */

    PORTA |= (1 << PA4);              /* SPI RAM deselected */
    DDRA |= (1 << PA4);               /* SPI RAM CS output */

    /* Start from a deterministic black front buffer without spending SRAM. */
    SpiRamSeqWriteStart(DISPLAY_BUFFER_BANK, DISPLAY_BUFFER_ADDR);
    for (uint16_t i = 0; i < DISPLAY_BYTES; ++i) {
        SpiRamSeqWriteU8(0);
    }
    SpiRamSeqWriteEnd();
}

/* sync_flags is owned by uzeboxVideoEngineCore.s. Keep the real-VSYNC callback
 * deliberately short. The full-frame assembly renderer owns presentation: it
 * either repeats SPI RAM or consumes a ready SRAM frame while simultaneously
 * refreshing the SPI backing copy.
 */
extern unsigned char sync_flags;

void VideoModeVsync(void) {
    /* Match the normal Uzebox video-mode contract: advance the kernel DDRC
     * fader exactly once per real VSYNC. This touches DDRC/fade state only; the
     * scanline renderer remains entirely in assembly. */
    ProcessFading();
    displayVsyncTick();

    uint8_t flags = sync_flags;
    flags |= SYNC_FLAG_VSYNC;
    flags ^= SYNC_FLAG_FIELD;
    sync_flags = flags;
}

void DisplayLogo(void) {
    /* No kernel logo renderer for this framebuffer mode. */
}
