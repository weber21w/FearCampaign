#ifndef FEARCAMPAIGN_HIGHSCORE_H
#define FEARCAMPAIGN_HIGHSCORE_H

#include <uzebox.h>

/* EEPROM reservation is build-specific. The JAMMA reservation list assigns
 * Fear Campaign slot/ID 8; ordinary Uzebox builds use the fixed application
 * ID below. */
#if FEARCAMPAIGN_JAMMA
#define FEARCAMPAIGN_EEPROM_ID 8u
#else
#define FEARCAMPAIGN_EEPROM_ID 0x4E55u
#endif
#define FEARCAMPAIGN_HIGHSCORE_COUNT 5u

void HighScore_Init(void);
u8 HighScore_Qualifies(uint32_t score);
void HighScore_BeginEntry(uint32_t score);
u8 HighScore_UpdateEntry(void); /* returns non-zero when entry was committed */
void HighScore_DrawTable(u8 highlightRank);
void HighScore_DrawEntry(void);
uint32_t HighScore_Score(u8 rank);
void HighScore_Initials(u8 rank,char out[4]);

#endif
