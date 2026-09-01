#ifndef FEARCAMPAIGN_POKEY_H
#define FEARCAMPAIGN_POKEY_H

#include <uzebox.h>

/* Missile Command sound-number bits from the original sound driver. */
#define POKEY_SLOABM 0x01u
#define POKEY_SEXPLO 0x02u
#define POKEY_SABLAU 0x04u
#define POKEY_SUNABM 0x08u
#define POKEY_SNEWAV 0x10u
#define POKEY_SENDGA 0x20u
#define POKEY_SBONUS 0x40u
#define POKEY_SNSHOT 0x80u

/* Synth waveform page selectors consumed directly by pokeyMixerCore.s. */
#define POKEY_WAVE_PURE   0u
#define POKEY_WAVE_NOISE0 1u
#define POKEY_WAVE_POLY5  2u
#define POKEY_WAVE_NOISE1 3u

void Pokey_Init(void);
void Pokey_SetMuted(u8 muted);
void Pokey_Start(u8 soundMask);
void Pokey_SetEngines(u8 flierActive,u8 smartActive);
void Pokey_ProcessMusic(void); /* one Uzebox field = four cabinet sound IRQ ticks */

/* The inline AVR mixer reads these directly. 16-bit arrays are little-endian
 * on AVR, so assembly accesses +0/+1 for low/high bytes. */
extern volatile u16 pokey_phase[4];
extern volatile u16 pokey_step[4];
extern volatile u8 pokey_wave[4];
extern volatile u8 pokey_amp[4];
extern volatile u16 pokey_filter; /* Q8.8 cabinet-style RC output state */
extern volatile u8 pokey_filter_extra; /* 0=/4, 1=/8, 3=/16 */
extern volatile u8 pokey_noise_page[3]; /* upper four bits of 4096-event poly17 positions */

#endif
