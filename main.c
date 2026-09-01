#include <uzebox.h>
#include "video.h"
#include "input.h"
#include "missile.h"
#include "pokey.h"

/*
 * Composite output remains 60 Hz while framebuffer publication is 30 Hz.
 * Cabinet logic and input remain interleaved with physical VSYNCs without
 * loading and saving the complete moving-object state on every field.
 *
 * Each rendered frame now has two phases:
 *
 *   field A (new SRAM frame is being consumed):
 *      sample input at VSYNC
 *      wait until active video retires
 *      refresh the 3200-byte SPI repeat framebuffer
 *      load the 366-byte gameplay state into displayBuffer
 *      execute one 60-Hz cabinet field
 *      KEEP that state resident in displayBuffer
 *
 *   field B (video repeats from SPI RAM):
 *      sample input at the next physical VSYNC
 *      execute the second 60-Hz cabinet field immediately from resident SRAM
 *      wait until active video retires
 *      snapshot/save gameplay state once
 *      draw one 30-Hz framebuffer and publish it
 *
 * Thus input and cabinet logic run once per physical field while gameplay
 * state crosses SPI only once per rendered frame. During field B the renderer
 * reads only SPI RAM, so foreground
 * is free to mutate displayBuffer as its gameplay workspace.
 */
int main(void){
    u16 lastVsync;
    u8 workResident=0u;

    initializeDisplay();
    Pokey_Init();
    Input_reset();
    Jamma_Init();
    Game_Init();

    Game_Draw();
    updateDisplay();
    lastVsync=GetVsyncCounter();

    for(;;){
        u16 now;

        do{now=GetVsyncCounter();}while(now==lastVsync);
        lastVsync=now;

        /* Hyperkin-safe foreground polling remains once per physical VSYNC. */
        Input_update();
        Jamma_Update();

        if(!workResident){
            /* The freshly published SRAM frame owns displayBuffer throughout
             * this active region. Once it retires, copy it to the SPI repeat
             * buffer and reclaim SRAM as the resident gameplay workspace. */
            displayWaitNextActiveEnd();
            waitDisplayBufferAvailable();
            Game_WorkAcquire();
            workResident=1u;

            Game_FieldResident();
            Input_consumePressed();
            /* Do not save or draw here. The complete moving state stays in
             * displayBuffer across the following physical VSYNC. */
            continue;
        }

        /* This field is being scanned from the stable SPI repeat framebuffer,
         * so displayBuffer is private gameplay RAM and the second 60-Hz logic
         * field can run immediately after the input sample. */
        Game_FieldResident();
        Input_consumePressed();

        /* SPI state save and framebuffer drawing must wait until the repeat
         * renderer has finished using the shared SPI bus. */
        displayWaitNextActiveEnd();
        Game_WorkReleaseForDraw();
        workResident=0u;
        Game_Draw();
        updateDisplay();
    }
}
