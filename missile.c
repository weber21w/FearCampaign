#include <string.h>
#include <stdint.h>
#include "missile.h"
#include "input.h"
#include "framebuffer.h"
#include "pokey.h"
#include "highscore.h"
#include <spiram.h>
#include <avr/pgmspace.h>
#include "data/logo.inc"

/* Decode the packed 160-pixel-wide title logo directly into the framebuffer.
 * Control byte: bit 7 selects repeat mode; low seven bits store count-1. */
static void drawLogo(u8 y){
    u16 si=0u,di=(u16)y*FB_STRIDE;
    const u16 end=(u16)(di+FEARCAMPAIGN_LOGO_RAW_BYTES);
    if(FEARCAMPAIGN_LOGO_WIDTH!=FB_WIDTH || (u16)y+FEARCAMPAIGN_LOGO_HEIGHT>FB_HEIGHT) return;
    while(si<FEARCAMPAIGN_LOGO_PACKED_BYTES && di<end){
        u8 ctl=pgm_read_byte(&fearLogoPacked[si++]);
        u8 count=(u8)((ctl&0x7fu)+1u);
        if(ctl&0x80u){
            u8 v=pgm_read_byte(&fearLogoPacked[si++]);
            while(count-- && di<end) displayBuffer[di++]=v;
        }else{
            while(count-- && di<end) displayBuffer[di++]=pgm_read_byte(&fearLogoPacked[si++]);
        }
    }
}

#define QSHIFT 8
#define QONE   (1u << QSHIFT)
/* Native arcade ABM motion: the side silos perform three normalized missile
 * steps per 60-Hz field; the center silo performs seven. Positions are true
 * unsigned 8.8, matching the cabinet coordinate representation. */
#define SIDE_INTERCEPT_SPEED   (3u*QONE)
#define CENTER_INTERCEPT_SPEED (7u*QONE)
#define ARCADE_LAUNCH_HIGH_Y   202u
#define ARCADE_LAUNCH_LOW_Y    180u
#define ARCADE_BATTERY_LAUNCH_Y 24u

enum { GAME_TITLE=0, GAME_PLAY, GAME_OVER, GAME_HISCORE, GAME_SCORES };
enum { ENEMY_ICBM=0, ENEMY_SMART=1 };
enum { FLIER_BOMBER=0, FLIER_SATELLITE=1 };
enum { WAVE_END_NONE=0, WAVE_END_ABM, WAVE_END_CITY, WAVE_END_PAUSE };


/* Attract mode is deterministic: the same POKEY seed drives the same enemy
 * sequence, while a tiny demo-only defender chooses a dangerous incoming
 * missile and steers the cursor toward it. This is intentionally separate from
 * normal gameplay and therefore adds no player-frame CPU cost. */
#define DEMO_DURATION_FIELDS 1800u
#define DEMO_POKEY_SEED 0x1357bUL
#define DEMO_FIRE_INTERVAL 40u
#define DEMO_AIM_LEAD_Y 24u

typedef struct __attribute__((may_alias)) {
    u16 x,y;
    s16 vx,vy;
    u8 originX,originY;
    u8 target;
    u16 life;
    u8 type;
    u8 active;
} EnemyMissile;

typedef struct __attribute__((may_alias)) {
    u16 x,y;
    s16 vx,vy;
    u8 originX;
    u8 targetX,targetY;
    u16 life;
    u8 active;
} Interceptor;

typedef struct __attribute__((may_alias)) {
    u8 x,y;       /* native arcade H/V coordinates */
    u8 r;         /* current native arcade radius, 0..13 */
    u8 ageType;   /* D7=offensive, D0..D6=arcade explosion stage 0..26 */
    u8 active;
} Explosion;

typedef struct __attribute__((may_alias)) {
    u16 x;
    s16 vx;
    u8 y;
    u8 type;
    u8 fireTimer; /* native horizontal dots traveled since last shot */
    u8 moveTimer; /* PLATIM: 0 moves now; reloads from PLARAT */
    u8 active;
} Flier;

typedef struct __attribute__((may_alias)) MissileWork {
    EnemyMissile enemyData[MISSILE_MAX_ENEMIES];
    Interceptor interceptorData[MISSILE_MAX_INTERCEPTORS];
    Explosion explosionData[MISSILE_MAX_EXPLOSIONS];
    Flier flier;
} MissileWork;

typedef char MissileWorkFitsFramebuffer[(sizeof(MissileWork)<=FB_BYTES)?1:-1];

/* Authoritative moving-object state stays in SPI RAM. Drawing uses a compact
 * AVR snapshot so there is no foreground SPI traffic while active video may
 * be replaying its backing framebuffer from the same bus. */
typedef struct { u8 originX,originYType,x,y; } EnemyDraw;
/* Interceptor screen Y and target Y are both <128, leaving one high bit in
 * each byte. Together those bits encode the 0..2 launch-battery index. */
typedef struct { u8 x,yOriginHi,targetX,targetYOriginLo; } InterceptorDraw;
typedef struct { u8 x,y,rPhase; } ExplosionDraw;
typedef struct __attribute__((may_alias)) {
    EnemyDraw enemy[MISSILE_MAX_ENEMIES];
    InterceptorDraw interceptor[MISSILE_MAX_INTERCEPTORS];
    ExplosionDraw explosion[MISSILE_MAX_EXPLOSIONS];
    u8 enemyCount,interceptorCount,explosionCount;
    u8 flierActive,flierX,flierY,flierType;
} MissileDrawState;

/* Compact render-only state remains separate from the SPI-backed moving-object
 * workspace so drawing does not need foreground SPI traffic. */
static MissileDrawState drawState;

#define MISSILE_STATE_BANK 0u
#define MISSILE_STATE_ADDR 0x1000u

/* On AVR the moving-state workspace always aliases displayBuffer. Avoid a
 * permanent SRAM pointer for an address that never changes. */
#define work ((MissileWork *)(void *)displayBuffer)

#define ENEMIES      (work->enemyData)
#define INTERCEPTORS (work->interceptorData)
#define EXPLOSIONS   (work->explosionData)
#define FLIER         (work->flier)

/* Rev-3 arcade progression. Wave 19 values repeat thereafter. */
static const u8 PROGMEM arcadeIcbmCount[19]={
    12,15,18,12,16,14,17,10,13,16,19,12,14,16,18,14,17,19,22
};
static const u8 PROGMEM arcadeSmartCount[19]={
    0,0,0,0,0,1,1,2,3,4,4,5,5,6,6,7,7,7,7
};
/* WICSPH:WICSPL from SETICS. This is the cabinet's global 8.8 number of
 * wait fields inserted between one-native-dot ICBM updates. A value of zero
 * means one update every 60-Hz field; 0x04d0 gives wave 1 its ~19.4 s descent. */
static const u16 PROGMEM arcadeIcbmWait60[15]={
    0x04d0,0x02e0,0x01c0,0x0108,0x00a0,0x0060,0x0040,0x0020,
    0x0010,0x000a,0x0006,0x0004,0x0002,0x0001,0x0000
};
/* OLDRAD/NEWRAD in the arcade produce these visible radii at five-field
 * intervals. 27 stages * 5 fields = 135 fields ~= 2.25 seconds. */
static const u8 PROGMEM arcadeExplosionRadius[27]={
    0,2,3,4,5,6,7,8,9,10,11,12,13,13,12,11,10,9,8,7,6,5,4,3,2,1,0
};
/* Native WSPLAU values: 60-Hz fields before another flier may activate. */
static const u8 PROGMEM flierCooldown60[7]={240,160,128,128,96,64,32};
/* Native horizontal distance between flier missile volleys (WSPFIR). */
static const u8 PROGMEM flierFireDistance[7]={128,96,64,48,32,32,16};

/* The arcade has ten palette sets, selected by (wave-1)/2 modulo 10. Our
 * 2bpp playfield has only four physical colors (0 black, 1 cyan, 2 red,
 * 3 yellow), so map the cabinet's background / city-backdrop / incoming /
 * player colors to the nearest available entry. This preserves the 20-wave
 * progression without touching the cycle-critical scanline renderer.
 *
 * Source palette roles are entries 0,1,2,7 from the rev-3 color table. */
static const u8 PROGMEM arcadePalette4[10][4]={
    {0,3,2,1}, /* #0 black / yellow / red / blue  */
    {0,3,1,1}, /* #1 black / yellow / green / blue */
    {0,1,2,1}, /* #2 black / blue / red / green */
    {0,2,3,1}, /* #3 black / red / yellow / blue */
    {1,3,2,0}, /* #4 blue / yellow / red / black */
    {1,3,2,1}, /* #5 cyan / yellow / red / blue */
    {2,1,0,3}, /* #6 magenta / green / black / yellow */
    {3,1,0,2}, /* #7 yellow / green / black / red */
    {3,2,2,1}, /* #8 white / red / magenta / green */
    {2,3,0,1}  /* #9 red / yellow / black / blue */
};
/* Starting arcade color for palette entry #4 in each set, expressed as the
 * 3-bit color number (hardware value >> 1). The IRQ increments the palette
 * every field, but only every other increment is visibly distinct. */
static const u8 PROGMEM arcadeFlashStart[10]={3,5,3,0,3,3,2,7,2,7};
/* Map the cabinet's 8 RGB combinations onto our four DAC colors in hardware
 * increment order: white,yellow,magenta,red,cyan,green,blue,black. */
static const u8 PROGMEM arcadeFlashMap[8]={3,3,2,2,1,1,1,0};

/* Ten precomposed 8x8 one-bit ammunition stockpiles. Zero ammo draws nothing.
 * Each missile is a two-pixel vertical mark; the states fill a balanced
 * 4+3+2+1 pyramid from the ground upward. One battery therefore costs one
 * tiny fixed-position mask blit per 30-Hz framebuffer build, independent of
 * how many missiles remain. */
static const u8 PROGMEM ammoPile8x8[10][8]={
    {0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x10}, /* 1 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x28}, /* 2 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x54,0x54}, /* 3 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xaa,0xaa}, /* 4 */
    {0x00,0x00,0x00,0x00,0x10,0x10,0xaa,0xaa}, /* 5 */
    {0x00,0x00,0x00,0x00,0x28,0x28,0xaa,0xaa}, /* 6 */
    {0x00,0x00,0x00,0x00,0x54,0x54,0xaa,0xaa}, /* 7 */
    {0x00,0x00,0x10,0x10,0x54,0x54,0xaa,0xaa}, /* 8 */
    {0x00,0x00,0x28,0x28,0x54,0x54,0xaa,0xaa}, /* 9 */
    {0x10,0x10,0x28,0x28,0x54,0x54,0xaa,0xaa}  /* 10 */
};
static const u8 PROGMEM ammoPileScreenX[3]={0u,64u,140u};

static void stateLoad(void){
    SpiRamReadInto(MISSILE_STATE_BANK,MISSILE_STATE_ADDR,work,sizeof(MissileWork));
}

static void stateSave(void){
    SpiRamWriteFrom(MISSILE_STATE_BANK,MISSILE_STATE_ADDR,work,sizeof(MissileWork));
}

static void stateReset(void){
    memset(work,0,sizeof(MissileWork));
    /* Do not write SPI here. newGame() can be entered from a resident 60-Hz
     * field while the video renderer is simultaneously replaying from SPI.
     * The reset state is committed after active video has retired. */
}

/* Original W3COMN target coordinates. Keeping the historical city index
 * order is harmless; rendering transforms each coordinate independently. */
static const u8 PROGMEM cityPhysX[MISSILE_CITY_COUNT]={95u,180u,148u,44u,71u,208u};
static const u8 PROGMEM cityPhysY[MISSILE_CITY_COUNT]={16u,21u,18u,18u,17u,17u};
static const u8 PROGMEM batteryPhysX[MISSILE_BATTERY_COUNT]={20u,123u,240u};
static const u8 PROGMEM batteryPhysY[MISSILE_BATTERY_COUNT]={22u,22u,22u};
/* Pretransformed static ground positions avoid two LPMs and a lookup for each
 * city/base every rendered frame. */
static const u8 PROGMEM cityScreenX[MISSILE_CITY_COUNT]={59u,112u,92u,27u,44u,130u};
static const u8 PROGMEM batteryScreenX[MISSILE_BATTERY_COUNT]={12u,77u,150u};

/* Native<->160x74 transforms are hot constant divisions. Snapshot capture
 * can perform 40+ of these in a dense
 * frame, and cursor->native conversion runs every logic field. 719 bytes of
 * flash lookup tables turn every transform into one LPM with zero SRAM cost. */
static const u8 PROGMEM physXScreenLut[256]={
    0u,1u,1u,2u,2u,3u,4u,4u,5u,6u,6u,7u,7u,8u,9u,9u,
    10u,11u,11u,12u,12u,13u,14u,14u,15u,16u,16u,17u,17u,18u,19u,19u,
    20u,21u,21u,22u,22u,23u,24u,24u,25u,26u,26u,27u,27u,28u,29u,29u,
    30u,31u,31u,32u,32u,33u,34u,34u,35u,36u,36u,37u,37u,38u,39u,39u,
    40u,41u,41u,42u,42u,43u,44u,44u,45u,46u,46u,47u,47u,48u,49u,49u,
    50u,51u,51u,52u,52u,53u,54u,54u,55u,55u,56u,57u,57u,58u,59u,59u,
    60u,60u,61u,62u,62u,63u,64u,64u,65u,65u,66u,67u,67u,68u,69u,69u,
    70u,70u,71u,72u,72u,73u,74u,74u,75u,75u,76u,77u,77u,78u,79u,79u,
    80u,80u,81u,82u,82u,83u,84u,84u,85u,85u,86u,87u,87u,88u,89u,89u,
    90u,90u,91u,92u,92u,93u,94u,94u,95u,95u,96u,97u,97u,98u,99u,99u,
    100u,100u,101u,102u,102u,103u,104u,104u,105u,105u,106u,107u,107u,108u,108u,109u,
    110u,110u,111u,112u,112u,113u,113u,114u,115u,115u,116u,117u,117u,118u,118u,119u,
    120u,120u,121u,122u,122u,123u,123u,124u,125u,125u,126u,127u,127u,128u,128u,129u,
    130u,130u,131u,132u,132u,133u,133u,134u,135u,135u,136u,137u,137u,138u,138u,139u,
    140u,140u,141u,142u,142u,143u,143u,144u,145u,145u,146u,147u,147u,148u,148u,149u,
    150u,150u,151u,152u,152u,153u,153u,154u,155u,155u,156u,157u,157u,158u,158u,159u
};
static const u8 PROGMEM physYScreenLut[256]={
    80u,79u,79u,79u,78u,78u,78u,77u,77u,77u,76u,76u,75u,75u,75u,74u,
    74u,74u,73u,73u,73u,72u,72u,71u,71u,71u,70u,70u,70u,69u,69u,69u,
    68u,68u,68u,67u,67u,66u,66u,66u,65u,65u,65u,64u,64u,64u,63u,63u,
    63u,62u,62u,61u,61u,61u,60u,60u,60u,59u,59u,59u,58u,58u,57u,57u,
    57u,56u,56u,56u,55u,55u,55u,54u,54u,54u,53u,53u,52u,52u,52u,51u,
    51u,51u,50u,50u,50u,49u,49u,48u,48u,48u,47u,47u,47u,46u,46u,46u,
    45u,45u,45u,44u,44u,43u,43u,43u,42u,42u,42u,41u,41u,41u,40u,40u,
    40u,39u,39u,38u,38u,38u,37u,37u,37u,36u,36u,36u,35u,35u,34u,34u,
    34u,33u,33u,33u,32u,32u,32u,31u,31u,31u,30u,30u,29u,29u,29u,28u,
    28u,28u,27u,27u,27u,26u,26u,26u,25u,25u,24u,24u,24u,23u,23u,23u,
    22u,22u,22u,21u,21u,20u,20u,20u,19u,19u,19u,18u,18u,18u,17u,17u,
    17u,16u,16u,15u,15u,15u,14u,14u,14u,13u,13u,13u,12u,12u,11u,11u,
    11u,10u,10u,10u,9u,9u,9u,8u,8u,8u,7u,7u,6u,6u,6u,5u,
    5u,5u,4u,4u,4u,3u,3u,3u,2u,2u,1u,1u,1u,0u,0u,0u,
    0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,
    0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u
};
static const u8 PROGMEM screenXPhysLut[160]={
    8u,8u,8u,8u,8u,8u,10u,11u,13u,14u,16u,18u,19u,21u,22u,24u,
    26u,27u,29u,30u,32u,34u,35u,37u,38u,40u,42u,43u,45u,47u,48u,50u,
    51u,53u,55u,56u,58u,59u,61u,63u,64u,66u,67u,69u,71u,72u,74u,75u,
    77u,79u,80u,82u,83u,85u,87u,88u,90u,91u,93u,95u,96u,98u,99u,101u,
    103u,104u,106u,107u,109u,111u,112u,114u,115u,117u,119u,120u,122u,123u,125u,127u,
    128u,130u,132u,133u,135u,136u,138u,140u,141u,143u,144u,146u,148u,149u,151u,152u,
    154u,156u,157u,159u,160u,162u,164u,165u,167u,168u,170u,172u,173u,175u,176u,178u,
    180u,181u,183u,184u,186u,188u,189u,191u,192u,194u,196u,197u,199u,200u,202u,204u,
    205u,207u,208u,210u,212u,213u,215u,217u,218u,220u,221u,223u,225u,226u,228u,229u,
    231u,233u,234u,236u,237u,239u,241u,242u,244u,245u,247u,247u,247u,247u,247u,247u
};
static const u8 PROGMEM screenYPhysLut[80]={
    206u,206u,206u,206u,206u,206u,205u,203u,200u,197u,194u,191u,189u,186u,183u,180u,
    177u,175u,172u,169u,166u,164u,161u,158u,155u,152u,150u,147u,144u,141u,138u,136u,
    133u,130u,127u,125u,122u,119u,116u,113u,111u,108u,105u,102u,100u,97u,94u,91u,
    88u,86u,83u,80u,77u,74u,72u,69u,66u,63u,61u,58u,55u,52u,49u,47u,
    45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u,45u
};

static u8 physXToScreen(u8 x){ return pgm_read_byte(&physXScreenLut[x]); }
static u8 physYToScreen(u8 y){ return pgm_read_byte(&physYScreenLut[y]); }
static u8 screenXToPhys(u8 x){
    if(x>=MISSILE_SCREEN_WIDTH) x=(MISSILE_SCREEN_WIDTH-1u);
    return pgm_read_byte(&screenXPhysLut[x]);
}
static u8 screenYToPhys(u8 y){
    if(y>=MISSILE_SCREEN_HEIGHT) y=(MISSILE_SCREEN_HEIGHT-1u);
    return pgm_read_byte(&screenYPhysLut[y]);
}
static u8 cityX(u8 i){ return pgm_read_byte(&cityPhysX[i]); }
static u8 cityY(u8 i){ return pgm_read_byte(&cityPhysY[i]); }
static u8 batteryX(u8 i){ return pgm_read_byte(&batteryPhysX[i]); }
static u8 batteryY(u8 i){ return pgm_read_byte(&batteryPhysY[i]); }
static u8 cityPos(u8 i){ return pgm_read_byte(&cityScreenX[i]); }
static u8 batteryPos(u8 i){ return pgm_read_byte(&batteryScreenX[i]); }

static uint32_t score;
/* Missile Command reads POKEY RANDOM directly. POKEY is clocked at 1.25 MHz
 * from the 10 MHz board clock; one 320x256 video field is exactly 20,480 POKEY
 * clocks. We reproduce its 17-bit polynomial and free-running field phase.
 * Exact byte-for-byte cabinet sequences would also require cycle-exact 6502
 * instruction timing between RANDOM reads. Within a field we use a fixed
 * 11-clock decorrelation stride; it is not claimed to reproduce instruction
 * timing at each individual source-code read site. */
static uint32_t pokeyFieldState;
static uint32_t pokeyReadState;
static uint32_t nextBonusScore;
static u8 wave;
static u8 cities;
static u8 cityPool;
static u8 batteries;
static u8 ammo[MISSILE_BATTERY_COUNT];
static u8 icbmsToSpawn;
static u8 smartsToSpawn;
static u8 waveDelay;       /* 60-Hz timer used by animated wave-end phases */
static u8 waveIntro;
static u8 waveBonusDone;   /* WAVE_END_* phase; name retained to avoid SRAM churn */
static u8 bonusCityMask;   /* visual survivor mask while cities count away */
static u16 waveBonusTotal; /* running on-screen wave bonus */
/* Front-end attract timing and the in-wave bonus subtotal are mutually
 * exclusive states. Overlay them to save two static SRAM bytes. */
static union { u16 bonusStage; u16 attract; } phaseTimer;
#define waveBonusStage phaseTimer.bonusStage
#define attractTimer   phaseTimer.attract
static u8 icbmTimerFrac;
static u8 icbmTimerWhole;
static u8 explosionPhase;
static u8 explosionAlloc;   /* EXPLOP: 0,19,18... rotating detonation slot */
static u8 citiesLostThisWave;
static u8 cleanupFast;      /* ALIVE: no ABMs/bangs/ammo, speed remaining offense */
static u8 suppressLaunches; /* TALIVE hopeless condition: cancel future offense */
static u8 flierCooldown;
static u8 gameState;
/* D0 is the ordinary pause flag. D7 is FREE FIRE for the current run.
 * The cheat sequence itself exists only on the title screen and reuses
 * waveDelay there, so it costs no dedicated SRAM byte. */
static u8 paused;
#define PAUSE_ACTIVE() ((u8)(paused&0x01u))
#define CHEAT_ACTIVE() ((u8)((paused&0x80u)!=0u))
#define CLEAR_CHEAT()  (paused=(u8)(paused&0x01u))
#define ARM_CHEAT()    (paused=(u8)(paused|0x80u))
static u8 flashPhase60;    /* visible palette cycle changes every two fields */
static u16 demoField;
static u8 demoFireCooldown;
static u8 demoMode;

static const u16 PROGMEM konamiCode[10]={
    INPUT_UP,INPUT_UP,INPUT_DOWN,INPUT_DOWN,INPUT_LEFT,
    INPUT_RIGHT,INPUT_LEFT,INPUT_RIGHT,INPUT_B,INPUT_A
};

/* Linear jump masks generated from MAME's documented POKEY poly17 step.
 * Applying the 20,480-clock transform once per virtual field avoids doing
 * 1.2 million 17-bit LFSR steps/sec on the AVR. */
static const uint32_t PROGMEM pokeyJumpField[17]={
    0x176dbUL,0x0ecb7UL,0x1d96eUL,0x1b3ddUL,0x166bbUL,0x0cd77UL,
    0x19aeeUL,0x134ddUL,0x068bbUL,0x0d176UL,0x1a3ecUL,0x147d9UL,
    0x08fb3UL,0x177ddUL,0x0eebbUL,0x1dd76UL,0x1bbedUL
};
static const uint32_t PROGMEM pokeyJumpRead[17]={
    0x00042UL,0x00084UL,0x00108UL,0x00210UL,0x00420UL,0x00840UL,
    0x01080UL,0x02000UL,0x04000UL,0x08000UL,0x10000UL,0x00001UL,
    0x00002UL,0x04004UL,0x08008UL,0x10010UL,0x00021UL
};
static uint32_t pokeyApplyJump(uint32_t state,const uint32_t *table){
    uint32_t out=0;
    u8 i;
    for(i=0u;i<17u;i++){
        if(state&1u) out^=pgm_read_dword(&table[i]);
        state>>=1;
    }
    return out&0x1ffffUL;
}
static void pokeyReset(void){
    /* poly17[0] after SKCTL is reset to 0 then enabled with 3. */
    pokeyFieldState=0x1ff7fUL;
    pokeyReadState=pokeyFieldState;
}
static void pokeyFieldTick(void){
    pokeyFieldState=pokeyApplyJump(pokeyFieldState,pokeyJumpField);
    pokeyReadState=pokeyFieldState;
}
static u8 rnd8(void){
    u8 v=(u8)(pokeyReadState>>8);
    pokeyReadState=pokeyApplyJump(pokeyReadState,pokeyJumpRead);
    return v;
}
static u8 abs8(s16 v){ return (u8)(v<0?-v:v); }
static u8 px(u16 q){ return (u8)(q>>QSHIFT); }
/* Missile Command's DELTA/GENVEL metric: max(delta)+3/8*min(delta), clamped
 * to one byte. It is used both for collision and velocity normalization. */
static u8 arcadeDistance(u8 dx,u8 dy){
    u16 lo=dx<dy?dx:dy;
    u16 hi=dx<dy?dy:dx;
    u16 d=hi+((lo*3u)>>3);
    return (u8)(d>255u?255u:d);
}
static u8 inArcadeRange(u8 x1,u8 y1,u8 x2,u8 y2,u8 range){
    u8 dx=abs8((s16)x1-(s16)x2);
    u8 dy=abs8((s16)y1-(s16)y2);
    if(dx>=range || dy>=range) return 0u;
    return arcadeDistance(dx,dy)<range;
}

static u8 scoreMultiplier(void){
    u8 m=(u8)((wave+1u)>>1);
    return m>6u?6u:m;
}

static void addScorePoints(uint32_t add){
    if(score>999999UL-add) score=999999UL;
    else score+=add;

    /* Default arcade bonus-city interval: 10,000 points. Bank reserves rather
     * than losing them when all six visible cities are currently intact. */
    while(nextBonusScore && score>=nextBonusScore){
        if(cityPool<99u) cityPool++;
        Pokey_Start(POKEY_SBONUS);
        if(nextBonusScore>=990000UL){ nextBonusScore=0; break; }
        nextBonusScore+=10000UL;
    }
}

static void addScore(u16 base){
    addScorePoints((uint32_t)base*(uint32_t)scoreMultiplier());
}

static void captureDrawState(void){
    u8 i,n;
    n=0;
    for(i=0;i<MISSILE_MAX_ENEMIES;i++) if(ENEMIES[i].active){
        EnemyDraw *d=&drawState.enemy[n++];
        d->originX=physXToScreen(ENEMIES[i].originX);
        d->originYType=(u8)(physYToScreen(ENEMIES[i].originY)|(ENEMIES[i].type<<7));
        d->x=physXToScreen(px(ENEMIES[i].x));
        d->y=physYToScreen(px(ENEMIES[i].y));
    }
    drawState.enemyCount=n;

    n=0;
    for(i=0;i<MISSILE_MAX_INTERCEPTORS;i++) if(INTERCEPTORS[i].active){
        InterceptorDraw *d=&drawState.interceptor[n++];
        u8 origin=(INTERCEPTORS[i].originX<70u)?0u:(INTERCEPTORS[i].originX<180u?1u:2u);
        u8 sy=physYToScreen(px(INTERCEPTORS[i].y));
        u8 ty=physYToScreen(INTERCEPTORS[i].targetY);
        d->x=physXToScreen(px(INTERCEPTORS[i].x));
        d->yOriginHi=(u8)(sy|((origin&2u)<<6));
        d->targetX=physXToScreen(INTERCEPTORS[i].targetX);
        d->targetYOriginLo=(u8)(ty|((origin&1u)<<7));
    }
    drawState.interceptorCount=n;

    n=0;
    for(i=0;i<MISSILE_MAX_EXPLOSIONS;i++) if(EXPLOSIONS[i].active){
        ExplosionDraw *d=&drawState.explosion[n++];
        u8 r=EXPLOSIONS[i].r;
        if(r>MISSILE_EXPLOSION_RADIUS) r=MISSILE_EXPLOSION_RADIUS;
        d->x=physXToScreen(EXPLOSIONS[i].x);
        d->y=physYToScreen(EXPLOSIONS[i].y);
        d->rPhase=(u8)(r|(((EXPLOSIONS[i].ageType&0x01u)!=0u?1u:0u)<<7));
    }
    drawState.explosionCount=n;

    drawState.flierActive=FLIER.active;
    drawState.flierX=FLIER.active?physXToScreen(px(FLIER.x)):0u;
    drawState.flierY=FLIER.active?physYToScreen(FLIER.y):0u;
    drawState.flierType=FLIER.type;
}

typedef struct {
    u8 enemies,smarts,icbms,interceptors,defExplosions,offExplosions;
    u8 highestEnemyY,explosions;
    s8 mirv;
} ObjectCensus;

/* Dense launch decisions used to rescan the 8 enemy slots and 20 explosion
 * slots several times in the same 60-Hz field. One census supplies every
 * POTENT/capacity count at once. It is stack/register scratch only: no new
 * persistent SRAM and no change to the authoritative SPI state. */
static void censusObjects(ObjectCensus *c){
    u8 i;
    c->enemies=c->smarts=c->icbms=c->interceptors=0u;
    c->defExplosions=c->offExplosions=c->explosions=0u;
    c->highestEnemyY=0u;
    c->mirv=-1;
    for(i=0u;i<MISSILE_MAX_ENEMIES;i++) if(ENEMIES[i].active){
        u8 y=px(ENEMIES[i].y);
        c->enemies++;
        if(y>c->highestEnemyY) c->highestEnemyY=y;
        if(ENEMIES[i].type==ENEMY_SMART) c->smarts++;
        else{
            c->icbms++;
            /* MIRVIX ultimately resolves to the lowest-numbered eligible
             * ICBM slot. The census already walks 0..7, so capture the first
             * one here instead of rescanning all missiles later. */
            if(c->mirv<0 && y>=MISSILE_ARCADE_MIRV_MIN_Y && y<MISSILE_ARCADE_MIRV_MAX_Y)
                c->mirv=(s8)i;
        }
    }
    for(i=0u;i<MISSILE_MAX_INTERCEPTORS;i++) if(INTERCEPTORS[i].active) c->interceptors++;
    for(i=0u;i<MISSILE_MAX_EXPLOSIONS;i++) if(EXPLOSIONS[i].active){
        c->explosions++;
        if(EXPLOSIONS[i].ageType&0x80u) c->offExplosions++;
        else c->defExplosions++;
    }
}

static u8 potentialFromCensus(const ObjectCensus *c){
    s16 offense=(s16)c->enemies+(FLIER.active?1:0)+(s16)c->offExplosions;
    s16 total=offense+(s16)c->interceptors+(s16)c->defExplosions;
    s16 byOffense=8-offense;
    s16 byTotal=20-total;
    s16 n=byOffense<byTotal?byOffense:byTotal;
    if(n<0) n=0;
    return (u8)n;
}

static u8 anyEnemies(void){
    u8 i;
    for(i=0;i<MISSILE_MAX_ENEMIES;i++) if(ENEMIES[i].active) return 1;
    return 0;
}
static void syncEngineSound(void){
    u8 i,smart=0u;
    for(i=0u;i<MISSILE_MAX_ENEMIES;i++){
        if(ENEMIES[i].active && ENEMIES[i].type==ENEMY_SMART){ smart=1u; break; }
    }
    Pokey_SetEngines(FLIER.active,smart);
}
static u8 canFireDefense(void){
    ObjectCensus c;
    u8 def,total;
    censusObjects(&c);
    def=(u8)(c.interceptors+c.defExplosions);
    total=(u8)(def+c.enemies+(FLIER.active?1u:0u)+c.offExplosions);
    return def<16u && total<20u;
}
static u8 anyInterceptors(void){
    u8 i;
    for(i=0;i<MISSILE_MAX_INTERCEPTORS;i++) if(INTERCEPTORS[i].active) return 1;
    return 0;
}
static u8 anyExplosions(void){
    u8 i;
    for(i=0;i<MISSILE_MAX_EXPLOSIONS;i++) if(EXPLOSIONS[i].active) return 1;
    return 0;
}

static u8 addExplosion(u8 x,u8 y,u8 offensive){
    Explosion *e;
    u8 slot;
    /* DETONA rejects centers at V>=210 (TOPSCR-EXDONE/2+1). The missile
     * itself still disappears; only the explosion is suppressed. */
    if(y>=210u) return 0u;

    /* EXPLOP is a rotating allocator: 0,19,18,...1,0. Potential-explosion
     * accounting guarantees that this slot is available when DETONA runs. */
    slot=explosionAlloc;
    explosionAlloc=(slot==0u)?(MISSILE_MAX_EXPLOSIONS-1u):(u8)(slot-1u);
    e=&EXPLOSIONS[slot];
    e->x=x; e->y=y;
    e->r=0u;
    e->ageType=(u8)(offensive?0x80u:0u);
    e->active=1u;
    Pokey_Start(POKEY_SEXPLO);
    return 1u;
}

static u8 targetX(u8 target){
    return target<MISSILE_CITY_COUNT?cityX(target):batteryX((u8)(target-MISSILE_CITY_COUNT));
}
static u8 targetY(u8 target){
    return target<MISSILE_CITY_COUNT?cityY(target):batteryY((u8)(target-MISSILE_CITY_COUNT));
}
static u8 bitCount6(u8 v){
    u8 n=0;
    v&=0x3fu;
    while(v){ n=(u8)(n+(v&1u)); v>>=1; }
    return n;
}

static const u8 PROGMEM ranbitMask[8]={0u,1u,3u,3u,7u,7u,7u,7u};
/* RANBIT: choose uniformly among set bits using the cabinet's mask/rejection
 * scheme rather than C modulo. Our bit 0 maps to the source's D7 target 0,
 * so scanning upward preserves the source target ordering. */
static u8 pickSetBit(u8 mask,u8 limit){
    u8 n=0u,i,pick,rmask;
    for(i=0u;i<limit;i++) if(mask&(u8)(1u<<i)) n++;
    if(!n) return 0u;
    rmask=pgm_read_byte(&ranbitMask[n-1u]);
    do{ pick=(u8)(rnd8()&rmask); }while(pick>=n);
    for(i=0u;i<limit;i++) if(mask&(u8)(1u<<i)){
        if(!pick) return i;
        pick--;
    }
    return 0u;
}
static u8 randomAnyTarget(void){
    u8 r=rnd8();
    return (u8)(((r>>1)&7u)+(r&1u)); /* ANYTHG */
}

static void targetedMasks(u8 *cityMask,u8 *batteryMask){
    u8 i,c=0u,b=0u;
    for(i=0u;i<MISSILE_MAX_ENEMIES;i++) if(ENEMIES[i].active){
        u8 t=ENEMIES[i].target;
        if(t<MISSILE_CITY_COUNT) c|=(u8)(1u<<t);
        else if(t<MISSILE_CITY_COUNT+MISSILE_BATTERY_COUNT)
            b|=(u8)(1u<<(t-MISSILE_CITY_COUNT));
    }
    *cityMask=c;
    *batteryMask=b;
}

/* GUICBM/RANBIT/NOTLCI target policy translated to our bit ordering. The key
 * invariant is CIDOWN + live-targeted-cities < 3 before another previously
 * untargeted live city may be exposed. Once that reaches three, new missiles
 * prefer untargeted live bases and finally dead/already-targeted city sites. */
static u8 chooseTarget(void){
    u8 cityT,batteryT,untCity,untBattery,pressure;
    targetedMasks(&cityT,&batteryT);
    untCity=(u8)(cities&(u8)~cityT&0x3fu);
    untBattery=(u8)(batteries&(u8)~batteryT&0x07u);
    pressure=(u8)(citiesLostThisWave+bitCount6((u8)(cityT&cities)));

    if(pressure<3u){
        if(untCity) return pickSetBit(untCity,MISSILE_CITY_COUNT);
        if(untBattery) return (u8)(MISSILE_CITY_COUNT+pickSetBit(untBattery,MISSILE_BATTERY_COUNT));
        return randomAnyTarget();
    }

    if(untBattery) return (u8)(MISSILE_CITY_COUNT+pickSetBit(untBattery,MISSILE_BATTERY_COUNT));
    /* Exact NOTLCI quirk. J occupies D7..D2; complementing the byte leaves
     * D1/D0 set, so the fallback can select left or middle base (targets 6/7)
     * regardless of their live/targeted state, but can never select right base 8. */
    {
        u8 cityAllowed=(u8)((u8)~untCity&0x3fu);
        u8 allowed8=(u8)(cityAllowed|0xc0u); /* local bits 6/7 => targets 6/7 */
        return pickSetBit(allowed8,8u);
    }
}

static s16 q8DeltaPerField(s16 delta,u16 fields){
    u16 mag,q;
    u8 neg=(u8)(delta<0);
    if(!fields) fields=1u;
    mag=(u16)(neg?-delta:delta);
    /* mag<=255, so mag<<8 fits exactly in unsigned 16 bits. This avoids a
     * 32-bit divide while retaining the cabinet's full 8 fractional bits. */
    q=(u16)(((u16)(mag<<8))/fields);
    return neg?(s16)-(s16)q:(s16)q;
}

static void startVector(u16 *x,u16 *y,s16 *vx,s16 *vy,u8 sx,u8 sy,u8 tx,u8 ty,u16 speed,u16 *life){
    s16 dx=(s16)tx-(s16)sx;
    s16 dy=(s16)ty-(s16)sy;
    u8 adx=abs8(dx),ady=abs8(dy);
    u16 dist=arcadeDistance(adx,ady);
    u16 frames;
    u8 nativeSpeed=(u8)(speed>>QSHIFT);
    if(!dist) dist=1u;
    if(!nativeSpeed) nativeSpeed=1u;
    frames=(u16)((dist+nativeSpeed-1u)/nativeSpeed);
    if(frames<1u) frames=1u;
    *x=(u16)((u16)sx<<QSHIFT);
    *y=(u16)((u16)sy<<QSHIFT);
    *vx=q8DeltaPerField(dx,frames);
    *vy=q8DeltaPerField(dy,frames);
    *life=frames;
}

static u16 icbmWaitValue(void){
    u8 w=wave>15u?15u:wave;
    if(cleanupFast) return 0u;
    return pgm_read_word(&arcadeIcbmWait60[w-1u]);
}
static u8 serviceIcbmsThisField(void){
    u16 wait,sum;
    if(icbmTimerWhole){ icbmTimerWhole--; return 0u; }
    wait=icbmWaitValue();
    sum=(u16)icbmTimerFrac+(u8)wait;
    icbmTimerFrac=(u8)sum;
    icbmTimerWhole=(u8)((wait>>8)+(sum>>8));
    return 1u;
}

static u8 spawnEnemyAt(u8 type,u8 sx,u8 sy){
    u8 i,t;
    for(i=0;i<MISSILE_MAX_ENEMIES;i++) if(!ENEMIES[i].active) break;
    if(i==MISSILE_MAX_ENEMIES) return 0;
    t=chooseTarget();
    ENEMIES[i].originX=sx;
    ENEMIES[i].originY=sy;
    ENEMIES[i].target=t;
    ENEMIES[i].type=type;
    startVector(&ENEMIES[i].x,&ENEMIES[i].y,&ENEMIES[i].vx,&ENEMIES[i].vy,
                sx,sy,targetX(t),targetY(t),QONE,&ENEMIES[i].life);
    ENEMIES[i].active=1;
    if(type==ENEMY_SMART) syncEngineSound();
    return 1;
}

static u8 cityCount(void){
    u8 i,n=0;
    for(i=0;i<MISSILE_CITY_COUNT;i++) if(cities&(1u<<i)) n++;
    return n;
}

static void regenerateCities(void){
    u8 want=cityPool>MISSILE_CITY_COUNT?MISSILE_CITY_COUNT:cityPool;
    u8 have=cityCount();
    while(have<want){
        u8 dead=(u8)((u8)~cities&0x3fu);
        u8 i;
        if(!dead) break;
        /* REGEN calls the same RANBIT routine used by target selection. */
        i=pickSetBit(dead,MISSILE_CITY_COUNT);
        cities|=(u8)(1u<<i);
        have++;
    }
}

static u8 waveTableIndex(void){ return wave>=19u?18u:(u8)(wave-1u); }
static u8 flierTableIndex(void){
    if(wave<=2u) return 0u;
    if(wave>=8u) return 6u;
    return (u8)(wave-2u);
}
static u8 flierCooldownValue(void){ return pgm_read_byte(&flierCooldown60[flierTableIndex()]); }
static u8 flierFireValue(void){ return pgm_read_byte(&flierFireDistance[flierTableIndex()]); }

static void beginWave(void){
    u8 i,wi=waveTableIndex();
    regenerateCities();
    batteries=0x07u;
    for(i=0;i<MISSILE_BATTERY_COUNT;i++) ammo[i]=MISSILE_BATTERY_AMMO;
    icbmsToSpawn=pgm_read_byte(&arcadeIcbmCount[wi]);
    smartsToSpawn=pgm_read_byte(&arcadeSmartCount[wi]);
    waveDelay=0u;
    /* Two cabinet seconds at 60-Hz logic. */
    waveIntro=120u;
    waveBonusDone=WAVE_END_NONE;
    bonusCityMask=cities;
    waveBonusTotal=0u;
    waveBonusStage=0u;
    citiesLostThisWave=0u;
    icbmTimerFrac=0u;
    icbmTimerWhole=0u;
    explosionPhase=0u;
    explosionAlloc=0u;
    flashPhase60=0u;
    cleanupFast=0u;
    suppressLaunches=0u;
    FLIER.active=0u;
    Pokey_SetEngines(0u,0u);
    /* SETICS initializes SPUTIM=SPUTAC, so the first flier of a wave is
     * immediately eligible. After it leaves/dies, the full delay applies. */
    flierCooldown=(wave>=2u)?0u:255u;
    Pokey_Start(POKEY_SNEWAV);
}


/* Classic title-screen code: U U D D L R L R B A. Console mode normally
 * treats any button as INSERT COIN/START, so the penultimate B is consumed
 * while a valid code prefix is in progress. The final A both arms FREE FIRE
 * and starts the console build; JAMMA still requires a real credit + START. */
static u8 updateTitleKonami(void){
    u16 p=(u16)(input.pressed&(INPUT_UP|INPUT_DOWN|INPUT_LEFT|INPUT_RIGHT|INPUT_A|INPUT_B));
    u16 expected;
    u8 pos=waveDelay; /* title-only scratch; beginWave() resets it before play */
    if(!p) return 0u;
    if(pos>=10u) pos=0u;
    expected=pgm_read_word(&konamiCode[pos]);
    if(p&expected){
        pos++;
        waveDelay=pos;
        if(pos>=10u){
            waveDelay=0u;
            ARM_CHEAT();
            Pokey_Start(POKEY_SBONUS);
            return 2u;
        }
        return (u8)(expected==INPUT_B || expected==INPUT_A);
    }
    waveDelay=(u8)((p&INPUT_UP)?1u:0u);
    return 0u;
}

static u8 userActivity(void){
    return (u8)(input.held || input.pressed || input.mouseHeld || input.mousePressed ||
                Input_mouseDX()!=0 || Input_mouseDY()!=0);
}

static void stopDemoToTitle(void){
    demoMode=0u; demoField=0u; demoFireCooldown=0u;
    CLEAR_CHEAT();
    Pokey_SetMuted(0u);
    gameState=GAME_TITLE;
    waveDelay=0u;
    attractTimer=0u;
    Input_centerCursor();
}

static void newGame(void){
    stateReset();
    score=0;
    nextBonusScore=10000UL;
    cityPool=MISSILE_CITY_COUNT;
    wave=1;
    cities=0x3fu;
    paused=(u8)(paused&0x80u);
    beginWave();
    gameState=GAME_PLAY;
    attractTimer=0u;
    Input_centerCursor();
}

static void startDemo(void){
    demoMode=1u;
    CLEAR_CHEAT();
    demoField=0u;
    demoFireCooldown=0u;
    /* Console attract remains silent.  JAMMA obeys the cabinet Softswitch
     * attract-sound DIP: Jamma_AttractSoundsEnabled() returns zero on the
     * ordinary console build and the configured cabinet value on JAMMA. */
    Pokey_SetMuted((u8)!Jamma_AttractSoundsEnabled());
    newGame();
    pokeyFieldState=DEMO_POKEY_SEED;
    pokeyReadState=DEMO_POKEY_SEED;
}

static void enemyImpact(EnemyMissile *m){
    u8 t=m->target;
    u8 wasSmart=(u8)(m->type==ENEMY_SMART);
    u8 x=targetX(t),y=targetY(t);
    m->active=0;
    if(wasSmart) syncEngineSound();
    /* Arcade offensive detonations use the same 0..13 explosion sequence.
     * Because these are below LOWEST, they cannot destroy other missiles. */
    addExplosion(x,y,1u);
    if(t<MISSILE_CITY_COUNT){
        if(cities&(1u<<t)){
            cities&=(u8)~(1u<<t);
            if(cityPool) cityPool--;
            if(citiesLostThisWave<3u) citiesLostThisWave++;
        }
    }else{
        t=(u8)(t-MISSILE_CITY_COUNT);
        if(t<MISSILE_BATTERY_COUNT){
            batteries&=(u8)~(1u<<t);
            ammo[t]=0;
        }
    }
}

static u16 interceptSpeedForBattery(u8 b){
    return b==1u?CENTER_INTERCEPT_SPEED:SIDE_INTERCEPT_SPEED;
}

static u8 offenseAlive(void){
    return (u8)(icbmsToSpawn || smartsToSpawn || anyEnemies() || FLIER.active);
}

static u8 fireInterceptorFromBattery(u8 b,u8 tx,u8 ty){
    u8 i,wasLow;
    u8 slotLimit=CHEAT_ACTIVE()?MISSILE_MAX_INTERCEPTORS:MISSILE_NORMAL_INTERCEPTORS;
    if(!offenseAlive() || !canFireDefense()) { Pokey_Start(POKEY_SNSHOT); return 0u; }
    if(b>=MISSILE_BATTERY_COUNT || !(batteries&(1u<<b)) || (!CHEAT_ACTIVE() && !ammo[b])) { Pokey_Start(POKEY_SNSHOT); return 0u; }
    if(tx<MISSILE_ARCADE_CURSOR_MIN_X) tx=MISSILE_ARCADE_CURSOR_MIN_X;
    if(tx>MISSILE_ARCADE_CURSOR_MAX_X) tx=MISSILE_ARCADE_CURSOR_MAX_X;
    if(ty<MISSILE_ARCADE_CURSOR_MIN_Y) ty=MISSILE_ARCADE_CURSOR_MIN_Y;
    if(ty>MISSILE_ARCADE_CURSOR_MAX_Y) ty=MISSILE_ARCADE_CURSOR_MAX_Y;
    /* Normal play preserves the cabinet-era eight-ABM in-flight limit.
     * FREE FIRE unlocks four extra interceptor slots for deliberate cheat-mode
     * missile spam without weakening the shared 20-object safety accounting. */
    for(i=0;i<slotLimit;i++) if(!INTERCEPTORS[i].active) break;
    if(i==slotLimit) { Pokey_Start(POKEY_SNSHOT); return 0u; }
    /* The cabinet substitutes its LOW-ABM warning when four rounds remain
     * before the decrement which leaves three visible missiles. */
    wasLow=0u;
    if(!CHEAT_ACTIVE()){
        wasLow=(u8)(ammo[b]==4u);
        ammo[b]--;
    }
    INTERCEPTORS[i].originX=batteryX(b);
    INTERCEPTORS[i].targetX=tx;
    INTERCEPTORS[i].targetY=ty;
    startVector(&INTERCEPTORS[i].x,&INTERCEPTORS[i].y,&INTERCEPTORS[i].vx,&INTERCEPTORS[i].vy,
                batteryX(b),ARCADE_BATTERY_LAUNCH_Y,tx,ty,
                interceptSpeedForBattery(b),&INTERCEPTORS[i].life);
    INTERCEPTORS[i].active=1;
    Pokey_Start(wasLow?POKEY_SLOABM:POKEY_SABLAU);
    return 1u;
}

static void fireInterceptor(u8 tx,u8 ty){
    u8 b,best=255,bestDist=255;
    for(b=0;b<MISSILE_BATTERY_COUNT;b++){
        u8 d;
        if(!(batteries&(1u<<b)) || (!CHEAT_ACTIVE() && !ammo[b])) continue;
        d=abs8((s16)tx-(s16)batteryX(b));
        if(d<bestDist){bestDist=d;best=b;}
    }
    if(best!=255u) (void)fireInterceptorFromBattery(best,tx,ty);
    else Pokey_Start(POKEY_SNSHOT);
}

/* Exact 16-heading geometry used by the cabinet CM avoidance code, converted
 * from the original 8.8 DEVIH/DEVIV vectors to Q7 unit vectors. Heading 0 is
 * straight up; headings increase counter-clockwise around the missile. */
static const s8 PROGMEM smartDirX16[16]={
      0,-49,-90,-118,-127,-118,-90,-49,  0,49,90,118,127,118,90,49
};
static const s8 PROGMEM smartDirY16[16]={
    127,118, 90,  49,   0, -49,-90,-118,-127,-118,-90,-49,0,49,90,118
};
/* Exact cabinet 8.8 evasive increments from $64ab-$64cf. These are used
 * only for movement; the compact s8 table above remains ideal for heading
 * dot-products and avoids 32-bit arithmetic there. */
static const s16 PROGMEM smartMoveXQ8[16]={
      0,-98,-180,-236,-256,-236,-180,-98,0,98,180,236,256,236,180,98
};
static const s16 PROGMEM smartMoveYQ8[16]={
    256,236,180,98,0,-98,-180,-236,-256,-236,-180,-98,0,98,180,236
};
static const u8 PROGMEM smartBigHole[16]={
    0x83,0x87,0x07,0x0f,0x0e,0x1e,0x1c,0x3c,
    0x38,0x78,0x70,0xf0,0xe0,0xe1,0xc1,0xc3
};
static const s8 PROGMEM smartProbeX[8]={0,-6,-8,-6,0,6,8,6};
static const s8 PROGMEM smartProbeY[8]={8,6,0,-6,-8,-6,0,6};

static u8 smartDangerMask(const EnemyMissile *m){
    u8 eidx,mask=0u;
    s16 mx=px(m->x),my=px(m->y);

    /* The literal cabinet algorithm probes eight points in the rendered bang.
     * Do the same geometry, but reject distant explosions once per explosion
     * instead of scanning all 20 explosions independently for every probe.
     * A probe is at most eight native dots from the CM center, so r+8 is an
     * exact rectangular coarse bound and cannot suppress a real danger bit. */
    for(eidx=0u;eidx<MISSILE_MAX_EXPLOSIONS && mask!=0xffu;eidx++){
        Explosion *e=&EXPLOSIONS[eidx];
        u8 i,reach;
        if(!e->active || !e->r) continue;
        reach=(u8)(e->r+8u);
        if(abs8(mx-(s16)e->x)>reach || abs8(my-(s16)e->y)>reach) continue;

        for(i=0u;i<8u;i++) if(!(mask&(u8)(1u<<i))){
            s16 tx=mx+(s8)pgm_read_byte(&smartProbeX[i]);
            s16 ty=my+(s8)pgm_read_byte(&smartProbeY[i]);
            u8 dx,dy;
            if(tx<0 || tx>255 || ty<0 || ty>255) continue;
            dx=abs8(tx-(s16)e->x);
            dy=abs8(ty-(s16)e->y);
            if(arcadeDistance(dx,dy)<=e->r) mask|=(u8)(1u<<i);
        }
    }
    return mask;
}

static u8 smartHeading(const EnemyMissile *m){
    s16 dx=(s16)targetX(m->target)-(s16)px(m->x);
    s16 dy=(s16)targetY(m->target)-(s16)px(m->y);
    s16 best=-32767;
    u8 i,bestI=0u;
    while(abs8(dx)>63u || abs8(dy)>63u){ dx>>=1; dy>>=1; }
    for(i=0;i<16u;i++){
        s8 ux=(s8)pgm_read_byte(&smartDirX16[i]);
        s8 uy=(s8)pgm_read_byte(&smartDirY16[i]);
        s16 dot=(s16)(dx*ux+dy*uy);
        if(dot>best){ best=dot; bestI=i; }
    }
    return bestI;
}

static s8 smartFindBigHole(u8 danger,u8 heading){
    u8 left=heading,right=heading,n;
    for(n=0u;n<9u;n++){
        if(!(danger&pgm_read_byte(&smartBigHole[left]))) return (s8)left;
        if(!(danger&pgm_read_byte(&smartBigHole[right]))) return (s8)right;
        left=(u8)((left-1u)&15u);
        right=(u8)((right+1u)&15u);
    }
    return -1;
}

static s8 smartFindSmallHole(u8 danger,u8 heading){
    u8 left=(u8)(heading>>1),right=left,n;
    for(n=0u;n<5u;n++){
        if(!(danger&(u8)(1u<<left))) return (s8)(left<<1);
        if(!(danger&(u8)(1u<<right))) return (s8)(right<<1);
        left=(u8)((left-1u)&7u);
        right=(u8)((right+1u)&7u);
    }
    return -1;
}

static void retargetEnemy(EnemyMissile *m){
    s16 dx=(s16)targetX(m->target)-(s16)px(m->x);
    s16 dy=(s16)targetY(m->target)-(s16)px(m->y);
    u16 dist=arcadeDistance(abs8(dx),abs8(dy));
    u16 frames;
    /* UPICBM advances every active missile by one normalized native-dot step
     * when the global ICBFR timer expires. Velocity therefore stays QONE; the
     * wave speed is entirely in serviceIcbmsThisField(). */
    if(!dist) dist=1u;
    frames=dist;
    m->vx=q8DeltaPerField(dx,frames);
    m->vy=q8DeltaPerField(dy,frames);
    m->life=frames;
}

/* CMNEWP/ANDANG/FINDBL/FINSIN behavior. The cabinet probes eight rendered
 * pixels around the cruise missile. Because this engine rebuilds its bitmap
 * after simulation, the same eight probes are evaluated from live explosion
 * geometry instead of reading the framebuffer. The danger mask and 16-way
 * big-hole/small-hole search are otherwise the original algorithm. */
static u8 smartEvade(EnemyMissile *m){
    u8 danger=smartDangerMask(m);
    u8 heading;
    s8 dir;
    s16 mx,my,stepX,stepY;
    if(!danger || danger==0xffu) return 0u;
    mx=px(m->x); my=px(m->y);
    if(mx<(s16)MISSILE_ARCADE_CURSOR_MIN_X || mx>(s16)MISSILE_ARCADE_CURSOR_MAX_X ||
       my<(s16)MISSILE_ARCADE_CURSOR_MIN_Y || my>(s16)MISSILE_ARCADE_CURSOR_MAX_Y) return 0u;

    heading=smartHeading(m);
    if(wave<9u) danger|=0x01u; /* STUPID: early CMs are forbidden to dodge up. */
    dir=smartFindBigHole(danger,heading);
    if(dir<0) dir=smartFindSmallHole(danger,heading);
    if(dir<0) return 0u;

    /* Use the cabinet's literal 8.8 DEVIH/DEVIV increments. Edge checks above
     * guarantee these signed deltas cannot underflow the unsigned positions. */
    stepX=(s16)pgm_read_word(&smartMoveXQ8[(u8)dir]);
    stepY=(s16)pgm_read_word(&smartMoveYQ8[(u8)dir]);
    m->x=(u16)(m->x+(u16)stepX);
    m->y=(u16)(m->y+(u16)stepY);
    if(m->x>(u16)(255u*QONE)) m->x=(u16)(255u*QONE);
    if(m->y>(u16)(MISSILE_ARCADE_TOP_Y*QONE)) m->y=(u16)(MISSILE_ARCADE_TOP_Y*QONE);
    retargetEnemy(m);
    return 1u;
}

static void activateFlier(void){
    u8 r=rnd8();
    u8 fromLeft=(u8)((r&0x80u)==0u);
    u8 type=(u8)(r&1u);
    u8 rv=rnd8();
    /* ACTPLA: three LSRs leave original bit 2 in carry for the ADC. */
    u16 y=(u16)(100u+(rv>>3)+((rv>>2)&1u));
    if(wave<6u) y+=32u;
    if(wave<4u) y+=16u;
    if(y>176u) y=176u;
    FLIER.type=type;
    FLIER.y=(u8)y;
    FLIER.x=(u16)((u16)(fromLeft?0u:255u)<<QSHIFT);
    FLIER.vx=(s16)(fromLeft?QONE:-QONE);
    FLIER.fireTimer=0u;
    FLIER.moveTimer=0u;
    FLIER.active=1u;
    syncEngineSound();
}

static void updateFlier(void){
    if(!FLIER.active){
        if(flierCooldown) flierCooldown--;
        return;
    }

    /* PROPLA: DEC PLATIM; a negative result moves one dot, then reloads
     * PLARAT[type] (1 bomber, 2 satellite). Starting at zero means an
     * immediate move, then one move every 2 or 3 60-Hz fields. */
    if(FLIER.moveTimer){
        FLIER.moveTimer--;
        return;
    }
    FLIER.moveTimer=(u8)(FLIER.type==FLIER_BOMBER?1u:2u);

    if((FLIER.vx>0 && px(FLIER.x)>=255u) || (FLIER.vx<0 && px(FLIER.x)==0u)){
        FLIER.active=0u;
        FLIER.fireTimer=0u;
        flierCooldown=flierCooldownValue();
        syncEngineSound();
        return;
    }
    FLIER.x=(u16)(FLIER.x+(u16)FLIER.vx);
    if(FLIER.fireTimer<255u) FLIER.fireTimer++;

    if((FLIER.vx>0 && px(FLIER.x)>=255u) || (FLIER.vx<0 && px(FLIER.x)==0u)){
        FLIER.active=0u;
        FLIER.fireTimer=0u;
        flierCooldown=flierCooldownValue();
        syncEngineSound();
    }
}

static u8 uniqueTopLaunchX(void){
    u8 tries,x,i,used;
    for(tries=0u;tries<32u;tries++){
        x=rnd8(); used=0u;
        for(i=0u;i<MISSILE_MAX_ENEMIES;i++){
            if(ENEMIES[i].active && ENEMIES[i].originY==MISSILE_ARCADE_TOP_Y && ENEMIES[i].originX==x){
                used=1u; break;
            }
        }
        if(!used) return x;
    }
    return rnd8();
}

static u8 launchIcbmFrom(u8 sx,u8 sy){
    if(!icbmsToSpawn) return 0u;
    if(!spawnEnemyAt(ENEMY_ICBM,sx,sy)) return 0u;
    icbmsToSpawn--;
    return 1u;
}

static u8 launchTopIcbm(void){
    return launchIcbmFrom(uniqueTopLaunchX(),MISSILE_ARCADE_TOP_Y);
}

static u8 launchSmart(void){
    if(!smartsToSpawn) return 0u;
    if(!spawnEnemyAt(ENEMY_SMART,uniqueTopLaunchX(),MISSILE_ARCADE_TOP_Y)) return 0u;
    smartsToSpawn--;
    return 1u;
}

static void arcadeLaunchScheduler(void){
    u8 potent,count,icbmOn,smartOn;
    s16 cap;
    s8 mirv;
    ObjectCensus c;
    u8 gate;
    if(suppressLaunches || !(icbmsToSpawn||smartsToSpawn)) return;

    /* One census supplies POTENT counts, the highest live ICBM gate,
     * explosion count and MIRV candidate without repeated object-array scans. */
    censusObjects(&c);
    {
        s16 g=(s16)ARCADE_LAUNCH_HIGH_Y-(s16)(wave*2u);
        if(g<(s16)ARCADE_LAUNCH_LOW_Y) g=ARCADE_LAUNCH_LOW_Y;
        gate=(u8)g;
    }
    if(c.highestEnemyY>gate) return;
    potent=potentialFromCensus(&c);
    if(!potent) return;

    /* ACTPLA is part of ICBLAU: the flier consumes one offensive potential
     * bang and can activate only while normal ICBMs remain to be launched. */
    if(icbmsToSpawn && !FLIER.active && wave>=2u && !flierCooldown){
        activateFlier();
        if(potent) potent--;
        if(!potent) return;
    }

    smartOn=c.smarts;
    if(smartsToSpawn && smartOn<3u && c.enemies<5u){
        /* CMLAUN: with both types remaining, one quarter of eligible launch
         * decisions are cruise missiles; if no ICBMs remain, force a CM. */
        if(!icbmsToSpawn || ((rnd8()&3u)==0u)){
            launchSmart();
            return;
        }
    }
    if(!icbmsToSpawn) return;

    icbmOn=c.icbms;
    smartOn=c.smarts;
    cap=(s16)8-(s16)(smartOn*2u)-(s16)icbmOn-(FLIER.active?1:0);
    if(cap<=0) return;
    count=(u8)cap;
    if(count>4u) count=4u;
    if(count>icbmsToSpawn) count=icbmsToSpawn;
    if(count>potent) count=potent;
    if(!count) return;

    /* ICNORM source priority is flier, then MIRV, then the top edge. A flier
     * or MIRV volley is capped at three children; top launches can use four. */
    if(FLIER.active && wave>=2u && FLIER.fireTimer>=flierFireValue()){
        u8 fx=px(FLIER.x);
        if(fx>=48u && fx<208u){
            /* SPUTFIR offsets the missile origin four dots ahead of the
             * moving aircraft's stored coordinate. */
            u8 sx=(u8)(FLIER.vx<0?(fx>=4u?fx-4u:0u):(fx<=251u?fx+4u:255u));
            if(count>3u) count=3u;
            FLIER.fireTimer=0u;
            while(count){ if(!launchIcbmFrom(sx,FLIER.y)) break; count--; }
            return;
        }
    }

    mirv=(c.explosions<12u)?c.mirv:-1;
    if(mirv>=0){
        EnemyMissile *parent=&ENEMIES[(u8)mirv];
        u8 sx=px(parent->x),sy=px(parent->y);
        if(count>3u) count=3u;
        while(count){ if(!launchIcbmFrom(sx,sy)) break; count--; }
        return;
    }

    while(count){ if(!launchTopIcbm()) break; count--; }
}

static void killFlier(void){
    u8 x=px(FLIER.x),y=FLIER.y;
    FLIER.active=0u;
    flierCooldown=flierCooldownValue();
    syncEngineSound();
    addScore(100u);
    addExplosion(x,y,1u);
}

static void updateExplosions(void){
    u8 i,j,start;
    /* PREXPL services one group of four slots per 60-Hz field. EXPFRA starts
     * at zero, so the first group is 16..19, then 12..15, 8..11, 4..7, 0..3. */
    if(explosionPhase==0u) explosionPhase=4u;
    else explosionPhase--;
    start=(u8)(explosionPhase*4u);

    i=(u8)(start+4u);
    while(i>start){
        Explosion *e;
        i--;
        e=&EXPLOSIONS[i];
        u8 stage,typeBit;
        if(!e->active) continue;

        stage=(u8)(e->ageType&0x7fu);
        typeBit=(u8)(e->ageType&0x80u);
        stage++;
        if(stage>=27u){
            e->active=0u;
            e->r=0u;
            continue;
        }
        e->ageType=(u8)(typeBit|stage);
        e->r=pgm_read_byte(&arcadeExplosionRadius[stage]);

        if(!e->r || e->y<MISSILE_ARCADE_LOWEST_DAMAGE_Y) continue;

        for(j=0;j<MISSILE_MAX_ENEMIES;j++){
            EnemyMissile *m=&ENEMIES[j];
            u8 mx,my,range;
            if(!m->active) continue;
            mx=px(m->x); my=px(m->y);
            range=(u8)(e->r+(m->type==ENEMY_SMART?3u:1u));
            if(inArcadeRange(mx,my,e->x,e->y,range)){
                u8 type=m->type;
                m->active=0u;
                if(type==ENEMY_SMART){ addScore(125u); syncEngineSound(); }
                else if(my>=MISSILE_ARCADE_LOWEST_DAMAGE_Y) addScore(25u);
                addExplosion(mx,my,1u);
            }
        }

        if(FLIER.active){
            u8 fx=px(FLIER.x);
            if(inArcadeRange(fx,FLIER.y,e->x,e->y,(u8)(e->r+6u))) killFlier();
        }
    }
}

static u8 totalAmmo(void){ return (u8)(ammo[0]+ammo[1]+ammo[2]); }

/* ENDWV1..ENDWV5 translated as an explicit visible tally. ABMs count away
 * one every six 60-Hz fields; cities one every eleven. Score is accumulated on
 * the bonus line and committed at each stage, matching the cabinet's UPSCOR
 * boundaries. */
static void beginWaveBonus(void){
    waveBonusDone=totalAmmo()?WAVE_END_ABM:(cities?WAVE_END_CITY:WAVE_END_PAUSE);
    waveDelay=0u;
    bonusCityMask=cities;
    waveBonusTotal=0u;
    waveBonusStage=0u;
    if(waveBonusDone==WAVE_END_PAUSE) waveDelay=60u;
}

static void commitWaveBonusStage(void){
    if(waveBonusStage){
        addScorePoints(waveBonusStage);
        waveBonusStage=0u;
    }
}

static void beginFinalGameOverFlow(void){
    attractTimer=0u;
    /* Arcade behavior: a qualifying score skips the iconic THE END blast and
     * goes directly to initials. THE END is only shown when the final score
     * does not make the high-score table. */
    if(!CHEAT_ACTIVE() && HighScore_Qualifies(score)){
        HighScore_BeginEntry(score);
        gameState=GAME_HISCORE;
    }else gameState=GAME_OVER;
}

static void updateWaveBonus60(void){
    if(waveBonusDone==WAVE_END_ABM){
        u8 b;
        if(waveDelay){ waveDelay--; return; }
        for(b=MISSILE_BATTERY_COUNT;b>0u;b--) if(ammo[b-1u]){
            u16 points=(u16)(5u*scoreMultiplier());
            ammo[b-1u]--;
            waveBonusStage=(u16)(waveBonusStage+points);
            waveBonusTotal=(u16)(waveBonusTotal+points);
            Pokey_Start(POKEY_SUNABM);
            waveDelay=5u; /* tick now, then five quiet fields = 6-field cadence */
            return;
        }
        commitWaveBonusStage();
        waveBonusDone=cities?WAVE_END_CITY:WAVE_END_PAUSE;
        if(cities) waveBonusTotal=0u; /* CLRTRI before the cabinet city tally */
        waveDelay=cities?0u:60u;
        return;
    }

    if(waveBonusDone==WAVE_END_CITY){
        u8 i;
        if(waveDelay){ waveDelay--; return; }
        for(i=0u;i<MISSILE_CITY_COUNT;i++) if(bonusCityMask&(u8)(1u<<i)){
            u16 points=(u16)(100u*scoreMultiplier());
            bonusCityMask&=(u8)~(1u<<i);
            waveBonusStage=(u16)(waveBonusStage+points);
            waveBonusTotal=(u16)(waveBonusTotal+points);
            Pokey_Start(POKEY_SUNABM);
            waveDelay=10u; /* 11-field cabinet city cadence */
            return;
        }
        commitWaveBonusStage();
        waveBonusDone=WAVE_END_PAUSE;
        waveDelay=60u;
        return;
    }

    if(waveBonusDone==WAVE_END_PAUSE){
        if(waveDelay){ waveDelay--; return; }
        /* The cabinet still tallies unused ABMs on a wave where the last city
         * was lost. That bonus can even cross the bonus-city threshold before
         * the game-over decision is made. */
        if(!cities && !cityPool){
            waveBonusDone=WAVE_END_NONE;
            Pokey_SetEngines(0u,0u); Pokey_Start(POKEY_SENDGA);
            if(demoMode){ stopDemoToTitle(); return; }
            beginFinalGameOverFlow();
            return;
        }
        if(wave<99u) wave++;
        beginWave();
    }
}

static void updatePlayerAliveState(void){
    u8 noAmmo=(u8)(totalAmmo()==0u);

    /* TALIVE first stops future offense only when every base is empty AND
     * either no city survives or three cities have already been lost this wave. */
    if(noAmmo && (!cities || citiesLostThisWave>=3u)){
        suppressLaunches=1u;
        icbmsToSpawn=0u;
        smartsToSpawn=0u;
    }

    /* With no ABMs, no defensive/offensive bangs, and no ammunition, ALIVE
     * requests a clean screen: ICBMs move every field and the flier's PLATIM
     * is forced to zero on every pass. Active offense is NOT deleted. */
    if(!cleanupFast && noAmmo && !anyInterceptors() && !anyExplosions()){
        cleanupFast=1u;          /* ALIVE is latched until the next wave */
        icbmTimerWhole=0u;       /* TALIVE writes ICBFRH=0 immediately */
    }
    if(cleanupFast && FLIER.active) FLIER.moveTimer=0u;
}


static u8 demoPointCovered(u8 tx,u8 ty){
    u8 i;
    /* Do not stack ABMs on a target that already has an interceptor heading
     * there or a useful defensive explosion nearby. This makes the attract
     * player look deliberate and preserves ammunition for later threats. */
    for(i=0u;i<MISSILE_MAX_INTERCEPTORS;i++){
        if(INTERCEPTORS[i].active && inArcadeRange(tx,ty,INTERCEPTORS[i].targetX,INTERCEPTORS[i].targetY,18u))
            return 1u;
    }
    for(i=0u;i<MISSILE_MAX_EXPLOSIONS;i++){
        Explosion *e=&EXPLOSIONS[i];
        if(e->active && !(e->ageType&0x80u) && e->r>=4u && inArcadeRange(tx,ty,e->x,e->y,(u8)(e->r+10u)))
            return 1u;
    }
    return 0u;
}

static u8 demoChooseTarget(u8 *tx,u8 *ty){
    u8 i,found=0u,bestY=255u,bestSmart=0u;
    u8 bx=128u,by=128u;

    /* Prefer the lowest (most dangerous) uncovered incoming missile. Smart
     * bombs win ties so the demo demonstrates that mechanic rather than
     * ignoring it. Aim a short distance ahead toward the ground, giving the
     * ABM time to arrive and creating useful chain-reaction zones. */
    for(i=0u;i<MISSILE_MAX_ENEMIES;i++){
        EnemyMissile *m=&ENEMIES[i];
        u8 mx,my,ay,isSmart;
        if(!m->active) continue;
        mx=px(m->x); my=px(m->y);
        /* Native Y decreases toward the ground. Lead downrange so the ABM can
         * arrive and its five-field-per-stage explosion can grow before impact. */
        ay=(u8)(my>(u8)(MISSILE_ARCADE_CURSOR_MIN_Y+DEMO_AIM_LEAD_Y)
                    ? my-DEMO_AIM_LEAD_Y : MISSILE_ARCADE_CURSOR_MIN_Y);
        if(ay>MISSILE_ARCADE_CURSOR_MAX_Y) ay=MISSILE_ARCADE_CURSOR_MAX_Y;
        if(mx<MISSILE_ARCADE_CURSOR_MIN_X) mx=MISSILE_ARCADE_CURSOR_MIN_X;
        if(mx>MISSILE_ARCADE_CURSOR_MAX_X) mx=MISSILE_ARCADE_CURSOR_MAX_X;
        if(demoPointCovered(mx,ay)) continue;
        isSmart=(u8)(m->type==ENEMY_SMART);
        if(!found || (isSmart&&!bestSmart) || (isSmart==bestSmart && my<bestY)){
            found=1u; bestSmart=isSmart; bestY=my; bx=mx; by=ay;
        }
    }

    if(!found && FLIER.active){
        bx=px(FLIER.x); by=FLIER.y;
        if(by<MISSILE_ARCADE_CURSOR_MIN_Y) by=MISSILE_ARCADE_CURSOR_MIN_Y;
        if(by>MISSILE_ARCADE_CURSOR_MAX_Y) by=MISSILE_ARCADE_CURSOR_MAX_Y;
        found=(u8)!demoPointCovered(bx,by);
    }
    if(found){ *tx=bx; *ty=by; }
    return found;
}

static void demoPlayback60(void){
    u8 tx,ty;
    s16 sx,sy,dx,dy;
    if(!demoMode) return;
    if(demoFireCooldown) demoFireCooldown--;

    if(demoChooseTarget(&tx,&ty)){
        /* Cursor motion remains human-looking and visibly tracks the chosen
         * intercept point instead of teleporting. */
        sx=physXToScreen(tx); sy=physYToScreen(ty);
        dx=(s16)sx-(s16)input.mouseX;
        dy=(s16)sy-(s16)input.mouseY;
        if(dx>3) input.mouseX=(u8)(input.mouseX+3u);
        else if(dx<-3) input.mouseX=(u8)(input.mouseX-3u);
        else input.mouseX=(u8)sx;
        if(dy>3) input.mouseY=(u8)(input.mouseY+3u);
        else if(dy<-3) input.mouseY=(u8)(input.mouseY-3u);
        else input.mouseY=(u8)sy;

        /* Fire only after the cursor has visibly settled near the target. The
         * normal nearest-battery chooser keeps the demonstration believable
         * and naturally spreads ammunition across the three bases. */
        if(!demoFireCooldown && abs8(dx)<=4u && abs8(dy)<=4u && !waveIntro && waveBonusDone==WAVE_END_NONE){
            fireInterceptor(tx,ty);
            demoFireCooldown=DEMO_FIRE_INTERVAL;
        }
    }else{
        /* No immediate threat: drift toward an upper-middle defensive area. */
        if(input.mouseX<80u) input.mouseX++; else if(input.mouseX>80u) input.mouseX--;
        if(input.mouseY<27u) input.mouseY++; else if(input.mouseY>27u) input.mouseY--;
    }

}

static void updatePlay60(u8 allowEdges,u8 moveJoyCursor){
    u8 i;
    /* Demo duration is wall-clock cabinet fields, including wave-intro and
     * end-wave bonus phases. This keeps the attract rotation predictable. */
    if(demoMode){
        demoField++;
        if(demoField>=DEMO_DURATION_FIELDS){ stopDemoToTitle(); return; }
    }
#if !FEARCAMPAIGN_JAMMA
    if(allowEdges && Input_pressed(INPUT_START)){ paused^=1u; return; }
#endif
    if(PAUSE_ACTIVE()) return;
    if(waveIntro){
        waveIntro--;
        if(demoMode) demoPlayback60();
        else if(moveJoyCursor) Input_updateCursor();
        return;
    }
    if(waveBonusDone!=WAVE_END_NONE){
        updateWaveBonus60();
        return;
    }

    /* Cabinet PLAY order: ICBLAU, ABMLAU, PROPLA, UPABMS, UPICBM, PREXPL,
     * UPCURS. This executes once per physical 60-Hz field so edge controls and
     * scheduler decisions remain interleaved at cabinet cadence. */
    arcadeLaunchScheduler();

    if(demoMode) demoPlayback60();
    else if(allowEdges){
        u8 tx=screenXToPhys(Input_cursorX());
        u8 ty=screenYToPhys(Input_cursorY());
#if FEARCAMPAIGN_JAMMA
        /* Cabinet P1 buttons 1/2/3 act as the three Missile Command launch
         * buttons. Button 4 is left unused for gameplay. */
        if(Input_pressed(INPUT_A)) fireInterceptorFromBattery(0u,tx,ty);
        if(Input_pressed(INPUT_B)) fireInterceptorFromBattery(1u,tx,ty);
        if(Input_pressed(INPUT_X)) fireInterceptorFromBattery(2u,tx,ty);
#else
        if(Input_primaryPressed() || Input_pressed(INPUT_B)) fireInterceptor(tx,ty);
        if(Input_mouseRightPressed()) fireInterceptorFromBattery(1u,tx,ty);
        if(Input_pressed(INPUT_X) || Input_pressed(INPUT_Y)) fireInterceptorFromBattery(1u,tx,ty);
        /* SL/SR are reserved for slow/fast joypad cursor motion. */
#endif
    }

    updateFlier();

    for(i=0;i<MISSILE_MAX_INTERCEPTORS;i++){
        Interceptor *m=&INTERCEPTORS[i];
        if(!m->active) continue;
        m->x=(u16)(m->x+(u16)m->vx);
        m->y=(u16)(m->y+(u16)m->vy);
        if(m->life) m->life--;
        if(!m->life){
            m->active=0u;
            addExplosion(m->targetX,m->targetY,0u);
        }
    }

    if(serviceIcbmsThisField()){
        for(i=0;i<MISSILE_MAX_ENEMIES;i++){
            EnemyMissile *m=&ENEMIES[i];
            u8 deviated=0u;
            if(!m->active) continue;
            if(m->type==ENEMY_SMART) deviated=smartEvade(m);
            if(!deviated){
                m->x=(u16)(m->x+(u16)m->vx);
                m->y=(u16)(m->y+(u16)m->vy);
                if(m->life) m->life--;
            }
            if(!m->life) enemyImpact(m);
        }
    }

    updateExplosions();
    if(moveJoyCursor) Input_updateCursor();

    updatePlayerAliveState();

    /* TALIVE can enter ENDWV immediately once the attack has been declared
     * hopeless and no plane/ABM/explosion remains. ENDWV1/CLEANU erases any
     * still-active ICBMs/cruise missiles rather than making the player watch
     * them crawl to impact. */
    if(suppressLaunches && !FLIER.active && !anyInterceptors() && !anyExplosions()){
        for(i=0u;i<MISSILE_MAX_ENEMIES;i++) ENEMIES[i].active=0u;
        syncEngineSound();
        beginWaveBonus();
        return;
    }

    if(!icbmsToSpawn && !smartsToSpawn && !anyEnemies() && !anyInterceptors() && !anyExplosions() && !FLIER.active)
        beginWaveBonus();
}

static void logicField60(u8 allowEdges,u8 moveJoyCursor){
    /* RANDOM is free-running independently of game state. Advance it once for
     * each virtual cabinet field before any game code can read it. */
    pokeyFieldTick();
    flashPhase60++;

    if(gameState==GAME_TITLE){
        u8 codeState;
        if(moveJoyCursor) Input_updateCursor();
        codeState=updateTitleKonami();
        /* A correct penultimate B must not trigger console INSERT COIN. The
         * final A is allowed through, starting FREE FIRE immediately there. */
        if(codeState!=1u && Jamma_StartRequested()){
            demoMode=0u;
            if(codeState!=2u) CLEAR_CHEAT();
            Pokey_SetMuted(0u); newGame();
        }
        else{
            if(userActivity()) attractTimer=0u;
            else if(attractTimer<65535u) attractTimer++;
            if(attractTimer>=480u){ CLEAR_CHEAT(); waveDelay=0u; gameState=GAME_SCORES; attractTimer=0u; }
        }
    }else if(gameState==GAME_SCORES){
        /* Initials are normally confirmed with A/LMB/START. Do not let that
         * still-held control immediately dismiss the score table on the next
         * field; first require a completely released/idle input field. */
        if(waveDelay){
            if(!userActivity()) waveDelay=0u;
        }else if(Jamma_StartRequested()){ demoMode=0u; CLEAR_CHEAT(); Pokey_SetMuted(0u); newGame(); }
        else if(userActivity()){ CLEAR_CHEAT(); waveDelay=0u; gameState=GAME_TITLE; attractTimer=0u; }
        else if(++attractTimer>=360u) startDemo();
    }else if(gameState==GAME_HISCORE){
        if(HighScore_UpdateEntry()){
            gameState=GAME_SCORES;
            attractTimer=0u;
            waveDelay=1u; /* wait for the confirming control to be released */
        }
    }else if(gameState==GAME_OVER){
        /* GAME_OVER is the cabinet's iconic THE END sequence. It runs for
         * 217 60-Hz fields: radius 1..109, then 108..1. The real cabinet only
         * reaches this state when the score did not qualify for initials. */
        if(Jamma_StartRequested()){ demoMode=0u; CLEAR_CHEAT(); Pokey_SetMuted(0u); newGame(); }
        else if(++attractTimer>=217u){
            attractTimer=0u;
            waveDelay=0u;
            gameState=GAME_SCORES;
        }
    }else{
        if(demoMode){
            /* A real start press interrupts playback immediately. Other
             * movement exits to the title without contaminating demo input. */
            if(Jamma_StartRequested()){ demoMode=0u; CLEAR_CHEAT(); Pokey_SetMuted(0u); newGame(); return; }
            if(userActivity()){ stopDemoToTitle(); return; }
            updatePlay60(0u,0u);
        }else updatePlay60(allowEdges,moveJoyCursor);
    }
}

void Game_Init(void){
    pokeyReset();
    HighScore_Init();
    gameState=GAME_TITLE;
    paused=0u;
    cities=0x3fu;
    cityPool=MISSILE_CITY_COUNT;
    batteries=7u;
    wave=1u;
    score=0u;
    nextBonusScore=10000UL;
    waveIntro=0u;
    waveDelay=0u;
    waveBonusDone=WAVE_END_NONE;
    bonusCityMask=0x3fu;
    waveBonusTotal=waveBonusStage=0u;
    icbmTimerFrac=icbmTimerWhole=explosionPhase=explosionAlloc=0u;
    cleanupFast=suppressLaunches=0u;
    flashPhase60=0u;
    attractTimer=demoField=0u;
    demoFireCooldown=demoMode=0u;
    stateReset();
    ammo[0]=ammo[1]=ammo[2]=MISSILE_BATTERY_AMMO;
}

void Game_WorkAcquire(void){
    /* Caller guarantees displayBuffer is no longer owned by the direct-SRAM
     * renderer and that any pending framebuffer backing copy is complete. */
    if(gameState==GAME_PLAY) stateLoad();
}

void Game_FieldResident(void){
    /* No SPI transactions here. This is deliberately safe to execute while a
     * repeat field is actively scanning the backing framebuffer from SPI RAM. */
    logicField60(1u,1u);
}

void Game_WorkReleaseForDraw(void){
    /* Caller waits for active video to retire before entering this function.
     * The render snapshot is now private SRAM; no HUD scanout ownership handoff
     * is required before captureDrawState(). */
    if(gameState==GAME_PLAY){
        captureDrawState();
        stateSave();
    }
}

static void drawCity(u8 i,u8 ground,u8 bg){
    s16 x=cityPos(i);
    if(!(cities&(1u<<i))){
        FB_HLine(x-4,MISSILE_GROUND_Y-1,8,ground);
        FB_Pixel(x-2,MISSILE_GROUND_Y-2,ground);
        FB_Pixel(x+2,MISSILE_GROUND_Y-2,ground);
        return;
    }
    FB_FillRect(x-5,MISSILE_GROUND_Y-4,3,3,ground);
    FB_FillRect(x-1,MISSILE_GROUND_Y-7,3,6,ground);
    FB_FillRect(x+3,MISSILE_GROUND_Y-5,3,4,ground);
    FB_Pixel(x,MISSILE_GROUND_Y-5,bg);
}

static void drawBattery(u8 i,u8 ground){
    s16 x=batteryPos(i);
    if(!(batteries&(1u<<i))){ FB_HLine(x-5,MISSILE_GROUND_Y-1,11,ground); return; }
    FB_LineInside((u8)(x-6),(u8)(MISSILE_GROUND_Y-1),(u8)x,(u8)(MISSILE_GROUND_Y-7),ground);
    FB_LineInside((u8)x,(u8)(MISSILE_GROUND_Y-7),(u8)(x+6),(u8)(MISSILE_GROUND_Y-1),ground);
    FB_HLine(x-6,MISSILE_GROUND_Y-1,13,ground);
    FB_VLine(x,MISSILE_GROUND_Y-9,3,3);
}

static void drawAmmoPile(u8 i,u8 count,u8 color){
    u8 x;
    if(i>=MISSILE_BATTERY_COUNT || !(batteries&(u8)(1u<<i)) || !count) return;
    if(count>MISSILE_BATTERY_AMMO) count=MISSILE_BATTERY_AMMO;
    x=pgm_read_byte(&ammoPileScreenX[i]);
    FB_BlitMask8_P(x,65u,&ammoPile8x8[count-1u][0],8u,color);
}

static void drawPlayScore(u8 bg,u8 color){
    uint32_t s=score;
    if(s>999999UL) s=999999UL;
    /* Keep the six-digit score out of the active play area. The ground is at
     * y=74, leaving exactly one 5-pixel font row beneath it; reserve only the
     * compact centered score island there. */
    FB_FillRect(67,75,26,5,bg);
    Font_Number(68,75,(u16)(s/1000UL),3,color);
    Font_Number(80,75,(u16)(s%1000UL),3,color);
}


/* Pre-rasterized first-quadrant screen offsets for each native arcade radius.
 * Each byte is DX:DY (4 bits each). The table is generated from the exact
 * max+3/8*min arcade-distance outline followed by the 160x74 anisotropic
 * transform. This avoids repeated distance searches, multiplies and constant
 * divisions
 * from the drawing path for only 111 bytes of flash. */
static const u8 PROGMEM ellipseOffset[15]={
    0,1,3,6,10,14,19,26,33,41,51,61,72,83,96
};
static const u8 PROGMEM ellipsePoint[96]={
    0x00,0x00,0x10,0x01,0x10,0x11,0x01,0x11,0x20,0x21,0x01,0x11,0x20,0x21,0x02,0x12,
    0x21,0x30,0x31,0x02,0x12,0x22,0x31,0x32,0x40,0x41,0x03,0x13,0x22,0x32,0x40,0x41,
    0x42,0x03,0x13,0x23,0x33,0x41,0x42,0x50,0x51,0x03,0x13,0x23,0x33,0x42,0x43,0x51,
    0x52,0x60,0x61,0x04,0x14,0x23,0x33,0x43,0x52,0x53,0x60,0x61,0x62,0x04,0x14,0x24,
    0x34,0x43,0x53,0x61,0x62,0x63,0x70,0x71,0x04,0x14,0x24,0x34,0x44,0x53,0x62,0x63,
    0x70,0x71,0x72,0x05,0x15,0x24,0x34,0x44,0x54,0x63,0x64,0x71,0x72,0x73,0x80,0x81
};

/* Fast packed-row writer for already-clipped explosion points. Build the
 * repeated 2bpp color once per ellipse/marker and select a constant bit mask
 * from X&3. This avoids AVR variable shifts for every explosion pixel. */
static inline u8 packed2bpp(u8 color){
    u8 c=(u8)(color&3u);
    c=(u8)(c|(c<<2));
    return (u8)(c|(c<<4));
}
static inline void putEllipsePixel(u8 *row,u8 x,u8 packed){
    u8 *dst=row+(x>>2);
    switch(x&3u){
        case 0u: *dst=(u8)((*dst&0x3fu)|(packed&0xc0u)); break;
        case 1u: *dst=(u8)((*dst&0xcfu)|(packed&0x30u)); break;
        case 2u: *dst=(u8)((*dst&0xf3u)|(packed&0x0cu)); break;
        default: *dst=(u8)((*dst&0xfcu)|(packed&0x03u)); break;
    }
}

static void drawArcadeEllipse(s16 cx,s16 cy,u8 radius,u8 color){
    u8 i,end;
    if(radius>MISSILE_EXPLOSION_RADIUS) radius=MISSILE_EXPLOSION_RADIUS;
    i=pgm_read_byte(&ellipseOffset[radius]);
    end=pgm_read_byte(&ellipseOffset[radius+1u]);

    /* Radius 13 transforms to at most 8x5 screen pixels. The overwhelming
     * majority of airborne explosions fit this interior box. Cache the center
     * row once and derive +/-dy rows from it; a 20-explosion chain otherwise
     * spends hundreds of calls recomputing framebuffer addresses. */
    if(cx>=8 && cx<=151 && cy>=5 && cy<=74){
        u8 *center=displayBuffer+(u16)(u8)cy*FB_STRIDE;
        u8 packed=packed2bpp(color);
        while(i<end){
            u8 p=pgm_read_byte(&ellipsePoint[i++]);
            u8 dx=(u8)(p>>4),dy=(u8)(p&15u);
            if(!dx){
                putEllipsePixel(center+(u8)(dy*FB_STRIDE),(u8)cx,packed);
                if(dy) putEllipsePixel(center-(u8)(dy*FB_STRIDE),(u8)cx,packed);
            }else if(!dy){
                putEllipsePixel(center,(u8)(cx+dx),packed);
                putEllipsePixel(center,(u8)(cx-dx),packed);
            }else{
                u8 off=(u8)(dy*FB_STRIDE);
                u8 *rowP=center+off,*rowM=center-off;
                putEllipsePixel(rowP,(u8)(cx+dx),packed);
                putEllipsePixel(rowP,(u8)(cx-dx),packed);
                putEllipsePixel(rowM,(u8)(cx+dx),packed);
                putEllipsePixel(rowM,(u8)(cx-dx),packed);
            }
        }
        return;
    }

    /* Edge explosions retain the generic clipped path exactly. */
    while(i<end){
        u8 p=pgm_read_byte(&ellipsePoint[i++]);
        s16 dx=(s16)(p>>4),dy=(s16)(p&15u);
        if(!dx){
            FB_Pixel(cx,cy+dy,color);
            if(dy) FB_Pixel(cx,cy-dy,color);
        }else if(!dy){
            FB_Pixel(cx+dx,cy,color);
            FB_Pixel(cx-dx,cy,color);
        }else{
            FB_Pixel(cx+dx,cy+dy,color); FB_Pixel(cx-dx,cy+dy,color);
            FB_Pixel(cx+dx,cy-dy,color); FB_Pixel(cx-dx,cy-dy,color);
        }
    }
}

static void drawTargetMarker(u8 x,u8 y,u8 color){
    u8 *center=displayBuffer+(u16)y*FB_STRIDE;
    u8 packed=packed2bpp(color);
    putEllipsePixel(center-(u8)(2u*FB_STRIDE),(u8)(x-2u),packed);
    putEllipsePixel(center-(u8)(2u*FB_STRIDE),(u8)(x+2u),packed);
    putEllipsePixel(center-FB_STRIDE,(u8)(x-1u),packed);
    putEllipsePixel(center-FB_STRIDE,(u8)(x+1u),packed);
    putEllipsePixel(center,x,packed);
    putEllipsePixel(center+FB_STRIDE,(u8)(x-1u),packed);
    putEllipsePixel(center+FB_STRIDE,(u8)(x+1u),packed);
    putEllipsePixel(center+(u8)(2u*FB_STRIDE),(u8)(x-2u),packed);
    putEllipsePixel(center+(u8)(2u*FB_STRIDE),(u8)(x+2u),packed);
}

static void drawFlier(u8 enemy,u8 flash){
    s16 x=(s16)drawState.flierX,y=(s16)drawState.flierY;
    if(!drawState.flierActive) return;
    if(drawState.flierType==FLIER_BOMBER){
        FB_HLine(x-4,y,9,enemy);
        FB_HLine(x-2,y-1,5,enemy);
        FB_Pixel(x,y-2,flash);
    }else{
        FB_HLine(x-3,y,7,enemy);
        FB_VLine(x,y-2,5,enemy);
        FB_Pixel(x-4,y,flash); FB_Pixel(x+4,y,flash);
    }
}

/* Original 8x8 down-arrow glyph (#28) from the cabinet symbol set.  Unlike
 * the ammo stockpiles, city centers are not 4-pixel aligned, so draw this
 * tiny intro-only glyph with clipped pixels rather than the aligned fast blit. */
static const u8 PROGMEM defendArrow8x8[8]={
    0x18u,0x3cu,0x7eu,0xffu,0xffu,0xbdu,0x3cu,0x3cu
};

static void drawDefendArrow(s16 x,s16 y,u8 color){
    u8 ry;
    for(ry=0u;ry<8u;ry++){
        u8 bits=pgm_read_byte(&defendArrow8x8[ry]),rx;
        for(rx=0u;rx<8u;rx++) if(bits&(u8)(0x80u>>rx))
            FB_Pixel((s16)(x+rx),(s16)(y+ry),color);
    }
}

/* ENDWV cabinet-style bonus glyphs. These are deliberately tiny and cheap:
 * the existing running bonus total is enough to derive how many items have
 * already been awarded, so the presentation costs no persistent state. */
static void drawBonusAbmGlyph(s16 x,s16 y,u8 color){
    FB_VLine(x,y,4,color);
    FB_Pixel(x-1,y+3,color);
    FB_Pixel(x+1,y+3,color);
}

static void drawBonusCityGlyph(s16 x,s16 y,u8 color,u8 bg){
    FB_FillRect(x,y+3,3,3,color);
    FB_FillRect(x+4,y+1,3,5,color);
    FB_FillRect(x+8,y+2,3,4,color);
    FB_Pixel(x+5,y+3,bg);
}

static void drawWaveBonusGlyphs(u8 player,u8 flash,u8 bg){
    u16 per;
    u8 awarded,i;

    if(waveBonusDone==WAVE_END_ABM){
        per=(u16)(5u*scoreMultiplier());
        awarded=per?(u8)(waveBonusTotal/per):0u;
        if(awarded>30u) awarded=30u;
        Font_TextP(55,45,PSTR("MISSILE BONUS"),player);
        for(i=0u;i<awarded;i++){
            s16 x=(s16)(37u+(u8)(i%15u)*6u);
            s16 y=(s16)(54u+(u8)(i/15u)*7u);
            drawBonusAbmGlyph(x,y,flash);
        }
    }else if(waveBonusDone==WAVE_END_CITY){
        per=(u16)(100u*scoreMultiplier());
        awarded=per?(u8)(waveBonusTotal/per):0u;
        if(awarded>MISSILE_CITY_COUNT) awarded=MISSILE_CITY_COUNT;
        Font_TextP(61,45,PSTR("CITY BONUS"),player);
        for(i=0u;i<awarded;i++)
            drawBonusCityGlyph((s16)(43u+i*12u),53,flash,bg);
    }
}

static void drawWorld(void){
    u8 i,set,bg,ground,enemy,player,flash;
    u8 cx=Input_cursorX(),cy=Input_cursorY();
    set=(u8)((((u8)(wave-1u))>>1)%10u);
    bg=pgm_read_byte(&arcadePalette4[set][0]);
    ground=pgm_read_byte(&arcadePalette4[set][1]);
    enemy=pgm_read_byte(&arcadePalette4[set][2]);
    player=pgm_read_byte(&arcadePalette4[set][3]);
    {
        u8 phase=(u8)(((flashPhase60>>1)+pgm_read_byte(&arcadeFlashStart[set]))&7u);
        flash=pgm_read_byte(&arcadeFlashMap[phase]);
    }
    if(cx<MISSILE_CURSOR_MIN_X) cx=MISSILE_CURSOR_MIN_X;
    if(cx>MISSILE_CURSOR_MAX_X) cx=MISSILE_CURSOR_MAX_X;
    if(cy<MISSILE_CURSOR_MIN_Y) cy=MISSILE_CURSOR_MIN_Y;
    if(cy>MISSILE_CURSOR_MAX_Y) cy=MISSILE_CURSOR_MAX_Y;
    FB_Clear(bg);
    FB_HLine(0,MISSILE_GROUND_Y,FB_WIDTH,ground);

    for(i=0;i<drawState.enemyCount;i++){
        EnemyDraw *m=&drawState.enemy[i];
        u8 type=(u8)(m->originYType>>7);
        if(type==ENEMY_SMART){
            FB_Pixel(m->x,m->y-1,flash);
            FB_HLine((s16)m->x-1,m->y,3,enemy);
            FB_Pixel(m->x,m->y+1,flash);
        }else{
            FB_LineInside(m->originX,(u8)(m->originYType&0x7fu),m->x,m->y,enemy);
            FB_Pixel(m->x,m->y,flash);
        }
    }
    for(i=0;i<drawState.interceptorCount;i++){
        InterceptorDraw *m=&drawState.interceptor[i];
        u8 origin=(u8)(((m->yOriginHi>>6)&2u)|(m->targetYOriginLo>>7));
        u8 sy=(u8)(m->yOriginHi&0x7fu);
        u8 ty=(u8)(m->targetYOriginLo&0x7fu);
        u8 ox=(origin==0u)?12u:(origin==1u?77u:150u);
        FB_LineInside(ox,71u,m->x,sy,player);
        FB_Pixel(m->x,sy,flash);
        /* Cursor bounds guarantee every persistent target marker is at least
         * two pixels inside the framebuffer, so avoid nine generic clipped
         * FB_Pixel() calls for every active interceptor. */
        drawTargetMarker(m->targetX,ty,flash);
    }
    for(i=0;i<drawState.explosionCount;i++){
        ExplosionDraw *e=&drawState.explosion[i];
        u8 r=(u8)(e->rPhase&0x1fu);
        if(!r) continue;
        drawArcadeEllipse(e->x,e->y,r,flash);
        if(r>4u) drawArcadeEllipse(e->x,e->y,(u8)(r-3u),flash);
    }
    drawFlier(enemy,flash);
    for(i=0;i<MISSILE_CITY_COUNT;i++){
        /* During the bonus tally, living cities already counted are erased
         * from the ground rather than turned into rubble. */
        if(waveBonusDone>=WAVE_END_CITY && (cities&(u8)(1u<<i)) && !(bonusCityMask&(u8)(1u<<i))) continue;
        drawCity(i,ground,bg);
    }
    for(i=0;i<MISSILE_BATTERY_COUNT;i++) drawBattery(i,ground);
    for(i=0;i<MISSILE_BATTERY_COUNT;i++) drawAmmoPile(i,ammo[i],ground);

    /* BASLOW / BASEMP persist beside each battery once its count reaches the
     * cabinet thresholds. Keep them above the ground and the physical stockpile. */
    if(waveBonusDone==WAVE_END_NONE){
        for(i=0;i<MISSILE_BATTERY_COUNT;i++){
            s16 bx=(s16)batteryPos(i);
            if(ammo[i]==0u) Font_TextP(bx-9,MISSILE_GROUND_Y-16,PSTR("EMPTY"),enemy);
            else if(ammo[i]<=3u) Font_TextP(bx-5,MISSILE_GROUND_Y-16,PSTR("LOW"),flash);
        }
    }

    if(!cleanupFast && waveBonusDone==WAVE_END_NONE &&
       (waveIntro || icbmsToSpawn || smartsToSpawn || drawState.enemyCount || drawState.flierActive)){
        FB_HLine((s16)cx-4,cy,9,player);
        FB_VLine(cx,(s16)cy-3,7,player);
        FB_Pixel(cx,cy,bg);
    }

    drawPlayScore(bg,ground);

    if(demoMode) Font_TextP(2,2,PSTR("DEMO"),1);

    if(PAUSE_ACTIVE()){
        Font_TextP(68,34,PSTR("PAUSED"),3);
    }else if(waveIntro){
        /* Rev-3 only prints DEFEND CITIES for the first three waves.  The
         * cabinet's message contains a deliberate seven-character gap, and
         * its dedicated 8x8 down-arrow glyph points at the cities.  Preserve
         * both visual cues at our 160x80 resolution. */
        if(wave<=3u){
            Font_TextP(42,42,PSTR("DEFEND       CITIES"),player);
            for(i=0u;i<MISSILE_CITY_COUNT;i++) if(cities&(u8)(1u<<i))
                drawDefendArrow((s16)cityPos(i)-4,55,player);
        }
    }else if(waveBonusDone!=WAVE_END_NONE){
        Font_TextP(52,25,PSTR("BONUS POINTS"),3);
        Font_Number(66,35,waveBonusTotal,5,1);
        drawWaveBonusGlyphs(player,flash,bg);
    }
}

/* Arcade FN_THE_END expands a special explosion from radius 1 through 109,
 * then contracts it back to zero. Because this renderer rebuilds the bitmap
 * from scratch at 30 Hz instead of accumulating octagon outlines in VRAM, we
 * draw the equivalent filled shape for the current 60-Hz radius. */
static u8 theEndRadius(void){
    u16 t=attractTimer;
    if(t<=108u) return (u8)(t+1u);
    if(t<=216u) return (u8)(217u-t);
    return 0u;
}

/* Exact first-octant relation used by the cabinet's 3/8-slope octagon is
 * x = radius - floor(3*y/8) until x/y cross. The upper octants are the
 * transposed half of the same curve. */
static u8 theEndHalfWidthNative(u8 radius,u8 dy){
    if(dy>radius) return 0u;
    if((u16)dy*11u <= (u16)radius*8u)
        return (u8)(radius-(u8)(((u16)dy*3u)>>3));
    return (u8)(((u16)(radius-dy)*8u+2u)/3u);
}

static void drawTheEndExplosion(u8 radius,u8 color){
    s16 y;
    if(!radius) return;
    for(y=0;y<FB_HEIGHT;y++){
        u8 sy=(u8)(y>=40?y-40:40-y);
        /* Native end blast is centered at (128,115) on a 256x231 screen. */
        u8 dy=(u8)(((u16)sy*231u+40u)/80u);
        u8 hxNative,hx;
        if(dy>radius) continue;
        hxNative=theEndHalfWidthNative(radius,dy);
        hx=(u8)(((u16)hxNative*160u+128u)/256u);
        FB_HLine((s16)(80-(s16)hx),y,(s16)(hx*2u+1u),color);
    }
}

/* Only five glyphs are needed for the end message. The arcade stretches
 * THE END dramatically; 4x horizontal / 5x vertical on our 160x80 bitmap
 * preserves essentially the same proportions after the 256x231->160x80
 * display reduction. */
static const u8 PROGMEM theEndGlyphs[5][5]={
    {7,2,2,2,2}, /* T */
    {5,5,7,5,5}, /* H */
    {7,4,6,4,7}, /* E */
    {5,7,7,7,5}, /* N */
    {6,5,5,5,6}  /* D */
};
static const char PROGMEM theEndText[]="THE END";

static u8 theEndGlyphIndex(char c){
    if(c=='T') return 0u;
    if(c=='H') return 1u;
    if(c=='E') return 2u;
    if(c=='N') return 3u;
    if(c=='D') return 4u;
    return 255u;
}

static void drawTheEndText(u8 color){
    u8 ci;
    s16 x=26;
    for(ci=0u;ci<7u;ci++,x+=16){
        char c=(char)pgm_read_byte(&theEndText[ci]);
        u8 gi=theEndGlyphIndex(c),ry;
        if(gi==255u) continue;
        for(ry=0u;ry<5u;ry++){
            u8 bits=pgm_read_byte(&theEndGlyphs[gi][ry]);
            u8 rx;
            for(rx=0u;rx<3u;rx++) if(bits&(u8)(4u>>rx))
                FB_FillRect((s16)(x+(s16)rx*4),
                            (s16)(27+(s16)ry*5),4,5,color);
        }
    }
}

static void drawTheEndScreen(void){
    u8 radius=theEndRadius();
    u8 flash=pgm_read_byte(&arcadeFlashMap[(flashPhase60>>1)&7u]);
    uint32_t s=score;
    if(s>999999UL) s=999999UL;

    /* The rev-3 cabinet turns the entire background red, then lets palette
     * entry #4 cycle through the end blast and score. */
    FB_Clear(2u);
    drawTheEndExplosion(radius,flash);
    if(s<1000UL){
        Font_Number(25,2,(u16)s,1u,flash);
    }else{
        u16 hi=(u16)(s/1000UL);
        u8 digits=(u8)(hi>=100u?3u:(hi>=10u?2u:1u));
        Font_Number(25,2,hi,1u,flash);
        Font_Number((s16)(25+(s16)digits*4),2,(u16)(s%1000UL),3u,flash);
    }

    /* FN_THE_END prints the message when expansion reaches radius 98. Once
     * printed in cabinet VRAM it persists while the blast contracts. */
    if(attractTimer>=97u && radius) drawTheEndText(2u);
}

void Game_Draw(void){
    if(gameState==GAME_TITLE){
        char hi[4];
        FB_Clear(0);
        /* 160x53 four-color FEAR CAMPAIGN logo, decoded directly from flash. */
        drawLogo(1u);
#if FEARCAMPAIGN_JAMMA
        if(Jamma_FreePlay() || Jamma_Credits()) Font_TextP(60,56,PSTR("PUSH START"),3);
        else Font_TextP(58,56,PSTR("INSERT COIN"),3);
#else
        /* Console build keeps the arcade wording; any actual button is the
         * equivalent of inserting a coin/starting a one-credit game. */
        Font_TextP(58,56,PSTR("INSERT COIN"),3);
#endif
        HighScore_Initials(0u,hi);
        /* Fixed-width "HI AAA 000000" is 51 pixels wide. */
        Font_TextP(54,65,PSTR("HI"),1); Font_Text(66,65,hi,1);
        Font_Number(82,65,(u16)(HighScore_Score(0u)/1000UL),3,3);
        Font_Number(94,65,(u16)(HighScore_Score(0u)%1000UL),3,3);
#if FEARCAMPAIGN_JAMMA
        if(!Jamma_FreePlay()){
            Font_TextP(112,65,PSTR("CR"),1);
            Font_Number(124,65,Jamma_Credits(),2,3);
        }
#endif
    }else if(gameState==GAME_SCORES){
        FB_Clear(0);
        HighScore_DrawTable(255u);
#if FEARCAMPAIGN_JAMMA
        Font_TextP(48,69,PSTR("CREDIT"),1); Font_Number(76,69,Jamma_Credits(),2,3);
#endif
    }else if(gameState==GAME_HISCORE){
        FB_Clear(0);
        HighScore_DrawEntry();
    }else if(gameState==GAME_OVER){
        drawTheEndScreen();
    }else drawWorld();

}

