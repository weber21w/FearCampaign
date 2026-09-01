# Fear Campaign

Fear Campaign is an Uzebox game based on the Atari Missile Command arcade rules and timing. The release uses a custom 160x80 2bpp framebuffer video mode, SPI RAM for framebuffer/game-state storage, and a four-channel POKEY-style audio path derived from the original Missile Command sound data.

## Build

From `FearCampaign/default/`:

```sh
make clean && make
```

For the JAMMA build:

```sh
make jamma
```

## Controls

### Uzebox

- Mouse or D-pad: move the targeting cursor.
- Left mouse / A / B: fire from the nearest available battery.
- Right mouse / X / Y: fire the center battery.
- SL: slow D-pad cursor movement.
- SR: fast D-pad cursor movement.
- START: pause/unpause during play.
- On title/attract screens, any actual button or mouse button starts the game.

The direct SNES mouse reader is used instead of the kernel SNES mouse path.

### JAMMA

- Joystick: move the targeting cursor.
- Buttons 1/2/3: fire left/center/right batteries.
- START: start when a credit is available, or immediately in free play.
- Service and coin inputs use the Uzebox JAMMA Rev-B mappings and Softswitch coin/credit settings.

Cheat: entering the Konami code on the title screen enables FREE FIRE for the run. FREE FIRE also raises the simultaneous player interceptor limit from 8 to 12.
