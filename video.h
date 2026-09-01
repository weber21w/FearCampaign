#ifndef FEARCAMPAIGN_VIDEO_H
#define FEARCAMPAIGN_VIDEO_H

#include "framebuffer.h"
#include "videoMode/videoModeConfig.h"

void initializeDisplay(void);
void updateDisplay(void);
void waitDisplayBufferAvailable(void);
void displayWaitNextActiveEnd(void);
void displayVsyncTick(void);

extern volatile u8 displayFrameReady;
extern volatile u8 displayActiveEpoch;
extern volatile u8 displayVsyncActiveEpoch;

#endif
