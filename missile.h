#ifndef FEARCAMPAIGN_MISSILE_H
#define FEARCAMPAIGN_MISSILE_H

#include <uzebox.h>
#include "missile_config.h"

void Game_Init(void);
/* Moving object state is loaded once, advanced across two physical 60-Hz
 * fields while resident in displayBuffer, then snapshotted/saved before the
 * 30-Hz framebuffer draw. */
void Game_WorkAcquire(void);
void Game_FieldResident(void);
void Game_WorkReleaseForDraw(void);

void Game_Draw(void);

#endif
