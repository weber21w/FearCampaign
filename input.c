#include "input.h"
#include "missile_config.h"

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <uzebox.h>

#define MOUSE_LEFT_RAW  0x0100u
#define MOUSE_RIGHT_RAW 0x0200u


InputState input;
#if FEARCAMPAIGN_JAMMA
static u8 jammaCoinEdge,jammaServiceEdge;
static u16 jammaPrevP1,jammaPrevP2;
#endif

static u8 clampCursorRange(s16 v,u8 lo,u8 hi){
    if(v<(s16)lo) return lo;
    if(v>(s16)hi) return hi;
    return (u8)v;
}

void Input_reset(void){
    input.held=0;
    input.pressed=0;
    input.mouseX=(u8)(MISSILE_SCREEN_WIDTH/2);
    input.mouseY=(u8)((MISSILE_CURSOR_MIN_Y+MISSILE_CURSOR_MAX_Y)/2u);
    input.mouseDX=0;
    input.mouseDY=0;
    input.mouseHeld=0;
    input.mousePressed=0;
    input.joySlowPhase=0;
#if FEARCAMPAIGN_JAMMA
    jammaCoinEdge=jammaServiceEdge=0u;
    jammaPrevP1=jammaPrevP2=0u;
#endif
}

/* Hyperkin-safe UzeVBP-style foreground reader. Keep the kernel controller
 * reader and SNES_MOUSE path disabled. These delays intentionally match the
 * hardware-tested direct reader: 8 waits after latch, 33 waits after each
 * clock, 8 waits between the standard and extended 16-bit words. */
#define WAIT_200NS() asm volatile("lpm\n\tlpm\n\t")
extern u16 joypad1_status_lo, joypad1_status_hi;
extern u16 joypad2_status_lo, joypad2_status_hi;

static void pollControllers(void){
    u8 i;
    joypad1_status_lo=joypad2_status_lo=0;
    joypad1_status_hi=joypad2_status_hi=0;

    JOYPAD_OUT_PORT |= _BV(JOYPAD_LATCH_PIN);
    for(i=0;i<8;i++) WAIT_200NS();
    JOYPAD_OUT_PORT &= (u8)~_BV(JOYPAD_LATCH_PIN);

    for(i=0;i<16;i++){
        joypad1_status_lo >>= 1;
        joypad2_status_lo >>= 1;
        JOYPAD_OUT_PORT &= (u8)~_BV(JOYPAD_CLOCK_PIN);
        if((JOYPAD_IN_PORT&_BV(JOYPAD_DATA1_PIN))==0) joypad1_status_lo|=(1u<<15);
        if((JOYPAD_IN_PORT&_BV(JOYPAD_DATA2_PIN))==0) joypad2_status_lo|=(1u<<15);
        JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
        for(u8 j=0;j<33;j++) WAIT_200NS();
    }

    if(joypad1_status_lo==(BTN_START+BTN_SELECT+BTN_Y+BTN_B) ||
       joypad2_status_lo==(BTN_START+BTN_SELECT+BTN_Y+BTN_B)) SoftReset();

    if(!((joypad1_status_lo|joypad2_status_lo)&MOUSE_SIGNATURE)) return;

    for(i=0;i<8;i++) WAIT_200NS();
    for(i=0;i<16;i++){
        joypad1_status_hi <<= 1;
        joypad2_status_hi <<= 1;
        JOYPAD_OUT_PORT &= (u8)~_BV(JOYPAD_CLOCK_PIN);
        if((JOYPAD_IN_PORT&_BV(JOYPAD_DATA1_PIN))==0) joypad1_status_hi|=1u;
        if((JOYPAD_IN_PORT&_BV(JOYPAD_DATA2_PIN))==0) joypad2_status_hi|=1u;
        JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
        for(u8 j=0;j<33;j++) WAIT_200NS();
    }
}

static u16 joypadBits(u16 joy){
    u16 next=0;
    if(joy&BTN_LEFT) next|=INPUT_LEFT;
    if(joy&BTN_RIGHT) next|=INPUT_RIGHT;
    if(joy&BTN_UP) next|=INPUT_UP;
    if(joy&BTN_DOWN) next|=INPUT_DOWN;
    if(joy&BTN_A) next|=INPUT_A;
    if(joy&BTN_B) next|=INPUT_B;
    if(joy&BTN_Y) next|=INPUT_Y;
    if(joy&BTN_X) next|=INPUT_X;
    if(joy&BTN_SL) next|=INPUT_L;
    if(joy&BTN_SR) next|=INPUT_R;
    if(joy&BTN_START) next|=INPUT_START;
    if(joy&BTN_SELECT) next|=INPUT_SELECT;
    return next;
}

static s8 signedMouseDelta(u8 v){
    u8 mag=(u8)(v&0x7fu);
    return (v&0x80u)?-(s8)mag:(s8)mag;
}

void Input_update(void){
    u16 raw[2],motion[2],next=0;
    u8 buttons=0;
    bool hasMouse=false;
    s16 mx=input.mouseX,my=input.mouseY;
    s16 totalDx=0,totalDy=0;

    pollControllers();
    raw[0]=joypad1_status_lo; raw[1]=joypad2_status_lo;
    motion[0]=joypad1_status_hi; motion[1]=joypad2_status_hi;

#if FEARCAMPAIGN_JAMMA
    {
        u16 coinNow=(u16)(raw[1]&(BTN_SL|BTN_SR));
        u16 serviceNow=(u16)(raw[0]&BTN_SL);
        if(coinNow && !(jammaPrevP2&(BTN_SL|BTN_SR))) jammaCoinEdge=1u;
        if(serviceNow && !(jammaPrevP1&BTN_SL)) jammaServiceEdge=1u;
        jammaPrevP1=raw[0]; jammaPrevP2=raw[1];
    }
#endif

    for(u8 port=0;port<2;port++){
        if(raw[port]&MOUSE_SIGNATURE){
            s8 dx,dy;
            hasMouse=true;
            if(raw[port]&MOUSE_LEFT_RAW) buttons|=MOUSE_BUTTON_LEFT;
            if(raw[port]&MOUSE_RIGHT_RAW) buttons|=MOUSE_BUTTON_RIGHT;
            dx=signedMouseDelta((u8)motion[port]);
            dy=signedMouseDelta((u8)(motion[port]>>8));
            totalDx+=dx; totalDy+=dy;
            mx+=dx; my+=dy;
        }else{
#if FEARCAMPAIGN_JAMMA
            /* JAMMA P1 is the gameplay control register. P2 SL/SR are coin
             * inputs, and P1 SL is service, so do not merge those as L/R. */
            if(port==0u){
                u16 b=joypadBits(raw[0]);
                b&=(u16)~(INPUT_L|INPUT_R);
                next|=b;
            }
#else
            /* Single-player console mode: a normal joypad in either port works. */
            next|=joypadBits(raw[port]);
#endif
        }
    }

    input.mouseDX=(s8)(totalDx<-127?-127:totalDx>127?127:totalDx);
    input.mouseDY=(s8)(totalDy<-127?-127:totalDy>127?127:totalDy);
    if(hasMouse){
        input.mouseX=clampCursorRange(mx,MISSILE_CURSOR_MIN_X,MISSILE_CURSOR_MAX_X);
        input.mouseY=clampCursorRange(my,MISSILE_CURSOR_MIN_Y,MISSILE_CURSOR_MAX_Y);
    }else{
        /* Keep the shared cursor coordinates: the joypad fallback owns them. */
        buttons=0;
        input.mouseDX=input.mouseDY=0;
    }

    input.pressed|=(u16)(next&(u16)~input.held);
    input.held=next;
    input.mousePressed|=(u8)(buttons&(u8)~input.mouseHeld);
    input.mouseHeld=buttons;
}

void Input_centerCursor(void){
    input.mouseX=(u8)(MISSILE_SCREEN_WIDTH/2);
    input.mouseY=(u8)((MISSILE_CURSOR_MIN_Y+MISSILE_CURSOR_MAX_Y)/2u);
    input.joySlowPhase=0u;
}

void Input_updateCursor(void){
    u16 dirs;
    u8 step=1u;
    s16 x,y;

    dirs=(u16)(input.held&(INPUT_LEFT|INPUT_RIGHT|INPUT_UP|INPUT_DOWN));
    if(!dirs){ input.joySlowPhase=0u; return; }

    /* Console shoulders are speed modifiers for the D-pad pointer:
     *   SL = 1 px every other 60-Hz field (30 px/s)
     *   none = 1 px every 60-Hz field       (60 px/s)
     *   SR = 2 px every 60-Hz field         (120 px/s)
     * If both are held, fall back to normal speed. Slow mode moves on its
     * first field so aiming still responds immediately. */
    if((input.held&INPUT_L) && !(input.held&INPUT_R)){
        input.joySlowPhase^=1u;
        if(!input.joySlowPhase) return;
    }else{
        input.joySlowPhase=0u;
        if((input.held&INPUT_R) && !(input.held&INPUT_L)) step=2u;
    }

    x=input.mouseX;
    y=input.mouseY;
    if(dirs&INPUT_LEFT) x-=step;
    if(dirs&INPUT_RIGHT) x+=step;
    if(dirs&INPUT_UP) y-=step;
    if(dirs&INPUT_DOWN) y+=step;
    input.mouseX=clampCursorRange(x,MISSILE_CURSOR_MIN_X,MISSILE_CURSOR_MAX_X);
    input.mouseY=clampCursorRange(y,MISSILE_CURSOR_MIN_Y,MISSILE_CURSOR_MAX_Y);
}

void Input_consumePressed(void){ input.pressed=0; input.mousePressed=0; }
s8 Input_mouseDX(void){ return input.mouseDX; }
s8 Input_mouseDY(void){ return input.mouseDY; }
bool Input_mouseLeftPressed(void){ return (input.mousePressed&MOUSE_BUTTON_LEFT)!=0; }
bool Input_mouseRightPressed(void){ return (input.mousePressed&MOUSE_BUTTON_RIGHT)!=0; }
u8 Input_cursorX(void){ return input.mouseX; }
u8 Input_cursorY(void){ return input.mouseY; }
bool Input_primaryPressed(void){ return Input_mouseLeftPressed()||Input_pressed(INPUT_A); }
bool Input_anyButtonPressed(void){
    const u16 buttons=(u16)(INPUT_A|INPUT_B|INPUT_START|INPUT_SELECT|INPUT_Y|INPUT_L|INPUT_R|INPUT_X);
    return (bool)(((input.pressed&buttons)!=0u) || input.mousePressed!=0u);
}

bool Input_jammaCoinPressed(void){
#if FEARCAMPAIGN_JAMMA
    u8 v=jammaCoinEdge; jammaCoinEdge=0u; return v!=0u;
#else
    return false;
#endif
}
bool Input_jammaServicePressed(void){
#if FEARCAMPAIGN_JAMMA
    u8 v=jammaServiceEdge; jammaServiceEdge=0u; return v!=0u;
#else
    return false;
#endif
}

/* JAMMA credit and cabinet policy. */
#if FEARCAMPAIGN_JAMMA
static u8 credits;
static u8 coinAccum;
static u8 coinMode;
static u8 attractSounds;
static const u8 PROGMEM coinsNeeded[7]={1,1,1,2,3,3,4};
static const u8 PROGMEM creditsAward[7]={1,2,3,1,2,4,3};
#define JAMMA_TABLE(a,i) pgm_read_byte(&(a)[i])
#endif

void Jamma_Init(void){
#if FEARCAMPAIGN_JAMMA
    credits=0u; coinAccum=0u; coinMode=0u; attractSounds=1u;
    /* Uzebox JAMMA Softswitch owns EEPROM block 43788 (0xAB0C).  Its
     * cabinet byte is universal across games:
     *   bit 0    cabinet type (upright/cocktail; Fear Campaign is horizontal)
     *   bits 1-3 coin/credit: 1/1,1/2,1/3,2/1,3/2,3/4,4/3,free play
     *   bit 4    attract sounds: 0=on, 1=off
     *   bits 5-7 reserved
     * All-zero is the documented default, hence 1C/1P + attract sound on if
     * Softswitch data is absent/unformatted. */
    {
        struct EepromBlockStruct b;
        if(EepromReadBlock(43788u,&b)==EEPROM_OK){
            u8 dip=(u8)b.data[0];
            coinMode=(u8)((dip>>1)&7u);
            attractSounds=(u8)((dip&0x10u)==0u);
        }
    }
#endif
}


#if FEARCAMPAIGN_JAMMA
static void addCredits(u8 n){
    u16 v=(u16)credits+n;
    credits=(u8)(v>99u?99u:v);
}
#endif

void Jamma_Update(void){
#if FEARCAMPAIGN_JAMMA
    if(Input_jammaServicePressed()) addCredits(1u);
    if(Input_jammaCoinPressed() && coinMode!=7u){
        coinAccum++;
        if(coinAccum>=JAMMA_TABLE(coinsNeeded,coinMode)){
            coinAccum=0u;
            addCredits(JAMMA_TABLE(creditsAward,coinMode));
        }
    }
#endif
}

u8 Jamma_StartRequested(void){
#if FEARCAMPAIGN_JAMMA
    if(!Input_pressed(INPUT_START)) return 0u;
    if(coinMode==7u) return 1u;
    if(!credits) return 0u;
    credits--;
    return 1u;
#else
    /* Console build presents the cabinet-style INSERT COIN screen, but any
     * actual button (including either mouse button) starts immediately. D-pad
     * motion alone does not accidentally leave attract mode or start a game. */
    return (u8)Input_anyButtonPressed();
#endif
}

u8 Jamma_Credits(void){
#if FEARCAMPAIGN_JAMMA
    return credits;
#else
    return 0u;
#endif
}
u8 Jamma_FreePlay(void){
#if FEARCAMPAIGN_JAMMA
    return coinMode==7u;
#else
    return 1u;
#endif
}


u8 Jamma_AttractSoundsEnabled(void){
#if FEARCAMPAIGN_JAMMA
    return attractSounds;
#else
    /* Console attract mode remains intentionally silent. */
    return 0u;
#endif
}
