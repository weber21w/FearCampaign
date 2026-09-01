#include <string.h>
#include <stdint.h>
#include "pokey.h"

#include <avr/pgmspace.h>

/* Missile Command hardware: 10 MHz master / 8 = 1.25 MHz POKEY.
 * The Uzebox inline PWM sample is one sample per 1820-cycle scanline. */
#define MC_POKEY_CLOCK_HZ 1250000UL
#define UZE_AUDIO_RATE_HZ 15734UL
#define POKEY_NORMAL_DIV  28UL

#define AUDC_NOTPOLY5 0x80u
#define AUDC_POLY4    0x40u
#define AUDC_PURE     0x20u
#define AUDC_VOLONLY  0x10u
#define AUDC_VOLUME   0x0fu

/* Required by the kernel sound-enable API; the POKEY mixer owns audio state. */
u8 sound_enabled;

volatile u16 pokey_phase[4];
volatile u16 pokey_step[4];
volatile u8 pokey_wave[4];
volatile u8 pokey_amp[4];
volatile u16 pokey_filter=0x8000u;
volatile u8 pokey_filter_extra=3u; /* bits request 3rd/4th arithmetic filter shifts */
/* Upper four bits of the 12-bit direct-poly17 event positions used by
 * channels 1/2/3. The low eight bits live in pokey_phase[].hi. */
volatile u8 pokey_noise_page[3];

#include "data/pokey_sound.inc"

static const u8 *point[8];
static u8 current[8];
static u8 frames[8];
static u8 count[8];
static u8 regs[8];
static u8 soundNumber;
static u8 engineFlags; /* bit0 flier/SPUTNIK, bit1 smart/cruise */
static u8 mbtime;
static u8 muted;
static uint32_t soundRandom;

/* MAME's current POKEY poly17 recurrence, used here only to randomize the
 * original bonus-city pitch. This runs a handful of C steps, never in HSYNC. */
static uint32_t poly17Step(uint32_t lfsr){
    uint32_t in8=((lfsr>>8)^(lfsr>>13))&1u;
    uint32_t in=lfsr&1u;
    lfsr>>=1;
    lfsr=(lfsr&0xff7fu)|(in8<<7);
    return ((in<<16)|lfsr)&0x1ffffu;
}
static u8 soundRand(void){
    u8 i;
    for(i=0u;i<37u;i++) soundRandom=poly17Step(soundRandom);
    return (u8)(soundRandom>>8);
}

/* Scale a timer-event rate into a 16-bit DDS increment without a 64-bit
 * multiply. 'scale' is 32768 for a pure square (two events per cycle), or
 * 256 for event-indexed polynomial pages. */
static u16 rateToStep(uint32_t rate,u16 scale){
    uint32_t q=rate/UZE_AUDIO_RATE_HZ;
    uint32_t r=rate%UZE_AUDIO_RATE_HZ;
    uint32_t v=q*(uint32_t)scale+
        (r*(uint32_t)scale+(UZE_AUDIO_RATE_HZ/2u))/UZE_AUDIO_RATE_HZ;
    return (u16)(v>65535UL?65535UL:v);
}

static void refreshChannel(u8 ch){
    u8 audf=regs[(u8)(ch*2u)];
    u8 audc=regs[(u8)(ch*2u+1u)];
    u8 vol=(u8)(audc&AUDC_VOLUME);
    uint32_t timerDen,rate;
    u16 scale;
    u8 wave;

    if(muted || !vol){
        pokey_amp[ch]=0u;
        pokey_step[ch]=0u;
        return;
    }

    /* AUDCTL is permanently $20 in Missile Command: only channel 3 uses the
     * 1.25-MHz clock directly. MAME models +4 borrow/reset clocks for an
     * unlinked high-clock channel; the other channels use /28 and AUDF+1. */
    if(ch==2u) timerDen=(uint32_t)audf+4u;
    else timerDen=POKEY_NORMAL_DIV*((uint32_t)audf+1u);
    rate=(MC_POKEY_CLOCK_HZ+(timerDen/2u))/timerDen;

    if(audc&AUDC_VOLONLY){
        /* Not used by the original Missile Command tables; make it DC-silent
         * rather than spending HSYNC cycles on a mode the game never requests. */
        pokey_amp[ch]=0u;
        pokey_step[ch]=0u;
        return;
    }
    if((audc&AUDC_NOTPOLY5) && (audc&AUDC_PURE)){
        wave=POKEY_WAVE_PURE;
        scale=32768u;
    }else if(!(audc&AUDC_NOTPOLY5) && (audc&AUDC_PURE)){
        wave=POKEY_WAVE_POLY5;
        scale=256u;
    }else{
        /* Missile Command does not use POLY4 controls. Two decorrelated
         * poly17 pages keep simultaneous explosion channels from phase-locking. */
        wave=(u8)((ch&1u)?POKEY_WAVE_NOISE1:POKEY_WAVE_NOISE0);
        scale=256u;
        (void)AUDC_POLY4;
    }
    pokey_wave[ch]=wave;
    pokey_step[ch]=rateToStep(rate,scale);
    pokey_amp[ch]=pgm_read_byte(&pokeyVolumeAmp[vol]);
}
static void refreshSynth(void){
    u8 ch;
    u8 load=0u;
    for(ch=0u;ch<4u;ch++){
        refreshChannel(ch);
        load=(u8)(load+pokey_amp[ch]);
    }
    /* Quantize the cabinet RC's level-dependent time constant. MAME's resistor
     * model makes quiet single voices much darker than loud/multi-voice output.
     * HSYNC implements these as alpha 1/16, 1/8, or 1/4 with fixed cycle cost. */
    if(load<=12u) pokey_filter_extra=3u;      /* two extra shifts: /16 */
    else if(load<=26u) pokey_filter_extra=1u; /* one extra shift:  /8  */
    else pokey_filter_extra=0u;               /* no extra shifts:   /4  */
}

void Pokey_Init(void){
    sound_enabled=1u;
    memset((void *)pokey_phase,0,sizeof(pokey_phase));
    memset((void *)pokey_step,0,sizeof(pokey_step));
    memset((void *)pokey_wave,0,sizeof(pokey_wave));
    memset((void *)pokey_amp,0,sizeof(pokey_amp));
    pokey_filter=0x8000u;
    pokey_filter_extra=3u;
    memset((void *)pokey_noise_page,0,sizeof(pokey_noise_page));
    memset(point,0,sizeof(point));
    memset(current,0,sizeof(current));
    memset(frames,0,sizeof(frames));
    memset(count,0,sizeof(count));
    memset(regs,0,sizeof(regs));
    soundNumber=0u; engineFlags=0u; mbtime=0u; muted=0u;
    soundRandom=0x1ffffu;
}

void Pokey_SetMuted(u8 value){
    u8 i;
    muted=(u8)(value!=0u);
    if(muted){ pokey_filter=0x8000u; pokey_filter_extra=3u; }
    if(!muted){ refreshSynth(); return; }
    for(i=0u;i<8u;i++){ point[i]=0; current[i]=0u; regs[i]=0u; frames[i]=0u; count[i]=0u; }
    engineFlags=0u; mbtime=0u; soundNumber=0u;
    refreshSynth();
}

void Pokey_Start(u8 mask){
    u8 s,r;
    if(muted || !mask) return;
    soundNumber=mask;
    /* Original SNDON chooses the highest asserted sound-number bit. */
    for(s=7u;s>0u;s--) if(mask&(u8)(1u<<s)) break;
    for(r=0u;r<8u;r++){
        const u8 *p=(const u8 *)(uintptr_t)pgm_read_word(&soundMap[s][r]);
        if(p){ point[r]=p; frames[r]=1u; count[r]=1u; }
    }
}

void Pokey_SetEngines(u8 flierActive,u8 smartActive){
    u8 old=engineFlags;
    u8 now=(u8)((flierActive?1u:0u)|(smartActive?2u:0u));
    if(muted) now=0u;
    engineFlags=now;
    if((now&2u) && !(old&2u)) mbtime=0x30u;      /* CMSNON */
    else if((now&1u) && !(old&1u) && !(now&2u)) mbtime=0x70u; /* STSNON */
    if(!now && !point[6]){ regs[6]=regs[7]=current[6]=current[7]=0u; refreshChannel(3u); }
}

/* The Missile Command board IRQs four times per video field (V=0,64,128,192),
 * and W3INT calls MODSND from that IRQ handler. Keep one MODSND-equivalent tick
 * separate so the Uzebox VSYNC wrapper advances exactly four cabinet IRQ ticks
 * per field. */
static void processSoundIrqTick(void){
    s8 x;
    for(x=7;x>=0;x--){
        u8 ux=(u8)x;
        const u8 *p;
        if(frames[ux]) frames[ux]--;
        if(frames[ux]){ regs[ux]=current[ux]; continue; }
        p=point[ux];
        if(!p){ regs[ux]=current[ux]; continue; }
        if(count[ux]) count[ux]--;
        if(count[ux]){
            u8 v;
            frames[ux]=pgm_read_byte(p-3);
            v=(u8)(current[ux]+pgm_read_byte(p-2));
            if((soundNumber&POKEY_SBONUS) && ux==0u){
                v=(u8)(soundRand()&0x1eu);
                if(!v) v=0x1eu;
            }
            current[ux]=v;
        }else{
            u8 start=pgm_read_byte(p);
            u8 fc=pgm_read_byte(p+1);
            current[ux]=start;
            frames[ux]=fc;
            if(!fc){
                point[ux]=0;
                if(ux==0u) current[ux]=0u;
            }else{
                count[ux]=pgm_read_byte(p+3);
                point[ux]=p+4;
            }
        }
        regs[ux]=current[ux];
    }

    /* PMRBIL: channel 4 carries the continuous flier/cruise engine whenever
     * a one-shot effect is not currently occupying AUDF4/AUDC4. */
    if(!point[6]){
        if(engineFlags){
            u8 top=(engineFlags==1u)?0x70u:0x30u;
            u8 bottom=(engineFlags==1u)?0x30u:0x00u;
            if(mbtime==bottom) mbtime=top;
            else if(mbtime>=2u) mbtime=(u8)(mbtime-2u);
            else mbtime=top;
            regs[6]=current[6]=mbtime;
            regs[7]=current[7]=0xa4u;
        }else{
            regs[6]=current[6]=0u;
            regs[7]=current[7]=0u;
        }
    }
}

void Pokey_ProcessMusic(void){
    u8 i;
    if(muted) return;
    /* Four real Missile Command sound IRQs occur per video field.  Advancing
     * all four here restores the original command/envelope/sweep duration
     * while keeping the cycle-critical HSYNC synthesizer unchanged. */
    for(i=0u;i<4u;i++) processSoundIrqTick();
    refreshSynth();
}

