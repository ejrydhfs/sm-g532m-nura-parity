# MT6735 UART / timer / APDMA parity audit

Status: audit-only, no patch in this run.

## Evidence checked
- Nura: `drivers/tty/serial/8250/8250_mtk.c`,
  `drivers/dma/mediatek/mtk-uart-apdma.c`, MTK timer/clocksource,
  `CONFIG_SERIAL_8250_MT6577`, `CONFIG_MTK_TIMER`, `CONFIG_MTK_UART_APDMA`.
- Downstream: `drivers/tty/serial/8250`, `drivers/misc/mediatek/uart*`,
  `drivers/misc/mediatek/timer`/GPT, APDMA.
- ROM symbols: `mtk_uart_*`, `mtk_gpt_*`, `mtk_uart_console_init`.

## Finding
The upstream 8250 MTK and APDMA drivers already cover this SoC family; the
board uses `&uart0` for UART-over-USB debug. Downstream adds vendor early-console
and DMA policy. No specific missing register sequence was established.

## Deferred work
- Validate APDMA on high baud rates and suspend/resume.
- Compare timer clock rate and earlycon behavior on hardware.

No patch is delivered because no concrete gap was proven.
