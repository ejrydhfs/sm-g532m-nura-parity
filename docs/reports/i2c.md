# MT6735 I2C parity report

## Files changed
- `drivers/i2c/busses/i2c-mt65xx.c`

Patch: `$M/patches/i2c.patch`

## Evidence
- Downstream: `drivers/i2c/busses/i2c-mt65xx.c`, `i2c-mtk.c`,
  `drivers/misc/mediatek/i2c/mt6735/{i2c.c,mt_i2c.h}`.
- ROM symbols: `i2c_common_probe`, `mt_i2c_irq`, `i2c_dma_write_mtk`, `mt_i2c_test`.

## Parity implemented
- Added `mediatek,mt6735-i2c`, `mediatek,mt6735m-i2c`, `mediatek,mt6753-i2c`
  compatibles with the correct v1 register layout, no PMIC I2C, no auto-restart,
  aux length register, and 32-bit APDMA.
- RS/repeated-start is only set for controllers that support auto-restart.
- Non-auto-restart controllers program `TRANSAC_LEN=1` per message instead of
  claiming a multi-message transaction.
- WRRD optimization is allowed for a write-then-read pair regardless of the
  auto-restart capability, matching the downstream transfer path.

## Compile
`drivers/i2c/busses/i2c-mt65xx.o` compiles cleanly (exit 0).

## Hardware-unverified
- Multi-message transfers on this exact controller.
- DMA boundary/length behavior and bus recovery.
- Clock-divider values at all bus speeds.
