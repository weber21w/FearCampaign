#ifndef FEARCAMPAIGN_FRAMEBUFFER_H
#define FEARCAMPAIGN_FRAMEBUFFER_H

#include <uzebox.h>
#include "videoMode/videoModeConfig.h"

#define FB_WIDTH  DISPLAY_WIDTH
#define FB_HEIGHT DISPLAY_HEIGHT
#define FB_STRIDE DISPLAY_STRIDE
#define FB_BYTES  DISPLAY_BYTES

extern u8 displayBuffer[FB_BYTES];

void FB_Clear(u8 color);
void FB_Pixel(s16 x,s16 y,u8 color);
void FB_HLine(s16 x,s16 y,s16 w,u8 color);
void FB_VLine(s16 x,s16 y,s16 h,u8 color);
void FB_LineInside(u8 x0,u8 y0,u8 x1,u8 y1,u8 color);
void FB_FillRect(s16 x,s16 y,s16 w,s16 h,u8 color);
void FB_BlitMask8_P(u8 x,u8 y,const u8 *rows,u8 h,u8 color);

/* Compact 3x5 UI font. Lowercase input is folded to uppercase. */
void Font_Text(s16 x,s16 y,const char *s,u8 color);
void Font_TextP(s16 x,s16 y,const char *s,u8 color);
void Font_Number(s16 x,s16 y,u16 value,u8 minDigits,u8 color);

#endif
