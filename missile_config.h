#ifndef FEARCAMPAIGN_CONFIG_H
#define FEARCAMPAIGN_CONFIG_H

/* 160x80 is the display framebuffer. Gameplay itself uses the original
 * Missile Command integer coordinate system and is transformed only at the
 * input/draw boundaries. */
#define MISSILE_SCREEN_WIDTH       160u
#define MISSILE_SCREEN_HEIGHT       80u
#define MISSILE_GROUND_Y            74u

/* Atari arcade gameplay coordinates (W3COMN). Vertical coordinates increase
 * upward: 222 is the top launch line, while city/base targets sit at 16..22. */
#define MISSILE_ARCADE_TOP_Y       222u
#define MISSILE_ARCADE_GROUND_Y     16u
#define MISSILE_ARCADE_Y_RANGE     (MISSILE_ARCADE_TOP_Y-MISSILE_ARCADE_GROUND_Y)
#define MISSILE_ARCADE_CURSOR_MIN_X  8u
#define MISSILE_ARCADE_CURSOR_MAX_X 247u
#define MISSILE_ARCADE_CURSOR_MIN_Y 45u
#define MISSILE_ARCADE_CURSOR_MAX_Y 206u
#define MISSILE_ARCADE_MIRV_MIN_Y  128u
#define MISSILE_ARCADE_MIRV_MAX_Y  160u
#define MISSILE_ARCADE_LOWEST_DAMAGE_Y 33u

/* Screen-space cursor limits are the arcade bounds after the native->160x74
 * transform. Keeping the cursor in this rectangle makes the mouse position,
 * ABM target, and rendered explosion center exactly agree. */
#define MISSILE_CURSOR_MIN_X         5u
#define MISSILE_CURSOR_MAX_X       154u
#define MISSILE_CURSOR_MIN_Y         6u
#define MISSILE_CURSOR_MAX_Y        64u

#define MISSILE_MAX_ENEMIES          8u
#define MISSILE_NORMAL_INTERCEPTORS  8u
#define MISSILE_MAX_INTERCEPTORS     12u
#define MISSILE_MAX_EXPLOSIONS      20u
#define MISSILE_CITY_COUNT           6u
#define MISSILE_BATTERY_COUNT        3u
#define MISSILE_BATTERY_AMMO        10u
#define MISSILE_EXPLOSION_RADIUS    13u

#endif
