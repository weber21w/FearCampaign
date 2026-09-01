#ifndef FEARCAMPAIGN_INPUT_H
#define FEARCAMPAIGN_INPUT_H

#include <stdbool.h>
#include <uzebox.h>

typedef struct __attribute__((packed)) InputState {
    u16 held;
    u16 pressed;
    /* Shared on-screen cursor position. Mouse deltas update it directly and
     * Input_updateCursor() also applies joypad D-pad motion, even when both
     * devices are connected. */
    u8 mouseX;
    u8 mouseY;
    s8 mouseDX;
    s8 mouseDY;
    /* Mouse buttons and the slow-cursor divider share one byte. SL/SR are
     * joypad speed modifiers; no acceleration state is kept. */
    u8 mouseHeld:2;
    u8 mousePressed:2;
    u8 joySlowPhase:1;
    u8 reservedInput0:3;
} InputState;
typedef char FearCampaignInputStateSizeCheck[(sizeof(InputState)==9u)?1:-1];

extern InputState input;

enum {
    INPUT_LEFT   = 0x0001,
    INPUT_RIGHT  = 0x0002,
    INPUT_UP     = 0x0004,
    INPUT_DOWN   = 0x0008,
    INPUT_A      = 0x0010,
    INPUT_B      = 0x0020,
    INPUT_START  = 0x0040,
    INPUT_SELECT = 0x0080,
    INPUT_Y      = 0x0100,
    INPUT_L      = 0x0200,
    INPUT_R      = 0x0400,
    INPUT_X      = 0x0800
};

enum {
    MOUSE_BUTTON_LEFT  = 0x01,
    MOUSE_BUTTON_RIGHT = 0x02
};

void Input_reset(void);
void Input_update(void);
void Input_updateCursor(void);
void Input_centerCursor(void);
void Input_consumePressed(void);

static inline bool Input_pressed(u16 b){ return (input.pressed & b)!=0; }
s8 Input_mouseDX(void);
s8 Input_mouseDY(void);
bool Input_mouseLeftPressed(void);
bool Input_mouseRightPressed(void);

/* Device-independent pointer API used by the game. */
u8 Input_cursorX(void);
u8 Input_cursorY(void);
bool Input_primaryPressed(void);
bool Input_anyButtonPressed(void);

/* Uzebox JAMMA Rev-B special inputs. The cabinet shift-register mapping used
 * by existing Uzebox JAMMA titles places service on P1 SL and the two coin
 * switches on P2 SL/SR. These are kept separate from gameplay buttons. */
bool Input_jammaCoinPressed(void);
bool Input_jammaServicePressed(void);

/* Cabinet/JAMMA policy. Console builds use the same API with free-start rules. */
void Jamma_Init(void);
void Jamma_Update(void);
u8 Jamma_StartRequested(void);
u8 Jamma_Credits(void);
u8 Jamma_FreePlay(void);
u8 Jamma_AttractSoundsEnabled(void);

#endif
