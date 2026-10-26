# S2MU005 flashlight LED driver

## Files
- `drivers/leds/flash/leds-s2mu005.c` (new)
- `drivers/leds/flash/Kconfig`
- `drivers/leds/flash/Makefile`
- `drivers/power/supply/s2mu005-charger.c` creates the `s2mu005-fled`
  platform device (child of the I2C charger/MUIC device).

Patch: `$M/patches/flash.patch` plus the updated `s2mu005.patch`.

## Evidence
- Downstream: `drivers/leds/leds_s2mu005.c`,
  `include/linux/leds-s2mu005.h`, `include/linux/mfd/samsung/s2mu005-private.h`.
- Register values: FLED_CH1_CTRL0=46, CH1_CTRL1=47, CH1_CTRL3=49,
  FLED_CTRL1=55, FLED_CTRL2=56, FLED_RSVD=60.
- Initialisation sequence and current macros copied from downstream
  `s2mu005_led_setup()`.
- Board DTS: `s2mu005_fled` node with rear flash/torch currents (900/175/
  175/75 mA) and GPIO 42/43.

## Implementation
- Standard LED flash class device `s2mu005:flash`.
- Torch brightness via `brightness_set_blocking` (15 steps, 25 mA each).
- Flash intensity via `flash_brightness_set` (25-1200 mA).
- Strobe on/off through FLED_RSVD I2C bits 0xc0; torch through bits 0x30.
- Flash timeout via CH1_CTRL3 (62 ms steps, max 992 ms).
- Boost setup: independent channels, 4.5 V flash boost, boost enable.
- No GPIO writes are needed in I2C mode; the downstream I2C path is used.

## Compile
`drivers/leds/flash/leds-s2mu005.o` and the updated
`drivers/power/supply/s2mu005-charger.o` compile cleanly.

## Hardware-unverified
- Actual LED current at the configured register steps.
- Flash timeout/strobe interaction and thermal/UVLO behavior.
- Front channel (not exposed; rear channel only).
