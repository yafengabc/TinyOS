#ifndef TINYOS_RP2350RV_MEMMAP_H
#define TINYOS_RP2350RV_MEMMAP_H

#include "types.h"

/* Memory map and clock constants for the RP2350's Hazard3 RISC-V cores.
 *
 * The chip is the same one board/rp2350/arm_memmap.h describes; what moves
 * is the CPU and the timer.  The ARM SysTick does not exist here: Hazard3
 * has the standard machine timer, with mtime/mtimecmp supplied by the SIO
 * block.  Every other peripheral (UART0, CLOCKS, RESETS, XOSC, IO_BANK0,
 * PADS_BANK0) is at the address the ARM port already uses. */

#define XIP_BASE        0x10000000u
#define SRAM_BASE       0x20000000u
#define SRAM_END        0x20082000u   /* 520 KiB, striped SRAM0..7 + 2x4K */

/* SIO: CPU id, GPIO, FIFO and the RISC-V machine timer.  SIO is at the
 * same address for both CPU architectures. */
#define SIO_BASE        0xd0000000u
#define SIO_CPUID       (SIO_BASE + 0x000u)

/* Hazard3 machine timer (RP2350 datasheet, SIO chapter).  mtime is free
 * running while MTIME_CTRL.EN is set; FULLSPEED makes it count at clk_sys
 * rather than the fixed 1 MHz rate, which is why the tick interval below is
 * in clk_sys cycles. */
#define SIO_MTIME_CTRL          (SIO_BASE + 0x1a4u)
#define SIO_MTIME               (SIO_BASE + 0x1b0u)
#define SIO_MTIMEH              (SIO_BASE + 0x1b4u)
#define SIO_MTIMECMP            (SIO_BASE + 0x1b8u)
#define SIO_MTIMECMPH           (SIO_BASE + 0x1bcu)
#define SIO_MTIME_CTRL_EN       (1u << 0)
#define SIO_MTIME_CTRL_FULLSPEED (1u << 1)

/* Boot ROM.  On RP2350 the RISC-V lookup entry is a 16-bit address of a
 * jump instruction stored at 0x7dfa (pico-sdk bootrom_constants.h:
 * BOOTROM_ENTRY_OFFSET - BOOTROM_WELL_KNOWN_PTR_SIZE), and it takes
 * (code, RT_FLAG_FUNC_RISCV = 1).  The ARM port instead uses the (code,
 * flags) pointer at 0x16 with RT_FLAG_FUNC_ARM_SEC. */
#define BOOTROM_RV_LOOKUP_PTR 0x7dfau
#define RT_FLAG_FUNC_RISCV    0x0001u

#define XOSC_BASE       0x40048000u
#define CLOCKS_BASE     0x40010000u
#define RESETS_BASE     0x40020000u
#define IO_BANK0_BASE   0x40028000u
#define PADS_BANK0_BASE 0x40038000u
#define UART0_BASE      0x40070000u
#define PLL_SYS_BASE    0x40050000u

/* RESETS: active-high reset bits, de-assert by clearing. */
#define RESETS_RESET_OFFSET      0x00u
#define RESETS_RESET_DONE_OFFSET 0x08u
#define RESET_UART0     (1u << 26)
#define RESET_PADS_BANK0 (1u << 9)
#define RESET_IO_BANK0  (1u << 6)
#define RESET_PLL_SYS   (1u << 14)

/* XOSC enable, exactly as board/rp2350/platform.c measures it: ENABLE is
 * 0xfab << 12 and FREQ_RANGE 0xaa0 selects the 1-15 MHz crystal range. */
#define XOSC_CTRL               (XOSC_BASE + 0x00u)
#define XOSC_STATUS             (XOSC_BASE + 0x04u)
#define XOSC_CTRL_ENABLE_12M    ((0xfabu << 12) | 0xaa0u)
#define XOSC_STATUS_STABLE      (1u << 31)

#define CLK_REF_CTRL_OFFSET     0x30u
#define CLK_REF_SELECTED_OFFSET 0x38u
#define CLK_SYS_CTRL_OFFSET     0x3cu
#define CLK_SYS_DIV_OFFSET      0x40u
#define CLK_SYS_SELECTED_OFFSET 0x44u
#define CLK_PERI_CTRL_OFFSET    0x48u
#define CLK_PERI_DIV_OFFSET     0x4cu

#define CLK_REF_CTRL_SRC_XOSC   0x2u
#define CLK_SYS_CTRL_SRC_AUX    0x1u
#define CLK_SYS_CTRL_AUX_PLL_SYS 0x0u
#define CLK_PERI_CTRL_ENABLE    (1u << 11)
#define CLK_PERI_CTRL_AUXSRC_CLK_SYS 0x0u
#define CLK_DIV_1               0x00010000u   /* 16.16 fixed point: 1.0 */

/* PLL_SYS: REFDIV 1, FBDIV 125 (12 MHz * 125 = 1500 MHz VCO), postdiv 5
 * and 2 = 150 MHz.  Same numbers the ARM port and pico-sdk use. */
#define PLL_SYS_CS_REG          (*(volatile uint32_t *)(PLL_SYS_BASE + 0x00u))
#define PLL_SYS_PWR_REG         (*(volatile uint32_t *)(PLL_SYS_BASE + 0x04u))
#define PLL_SYS_FBDIV_REG       (*(volatile uint32_t *)(PLL_SYS_BASE + 0x08u))
#define PLL_SYS_PRIM_REG        (*(volatile uint32_t *)(PLL_SYS_BASE + 0x0cu))
#define PLL_SYS_CS_LOCK         0x80000000u
#define PLL_SYS_PWR_PD          0x00000001u
#define PLL_SYS_PWR_VCOPD       0x00000020u
#define PLL_SYS_PWR_POSTDIVPD   0x00000008u

/* Frequency counter (FC0): used to confirm clk_sys after the PLL switch. */
#define FC0_REF_KHZ      (CLOCKS_BASE + 0x8cu)
#define FC0_MIN_KHZ      (CLOCKS_BASE + 0x90u)
#define FC0_MAX_KHZ      (CLOCKS_BASE + 0x94u)
#define FC0_INTERVAL     (CLOCKS_BASE + 0x9cu)
#define FC0_SRC          (CLOCKS_BASE + 0xa0u)
#define FC0_STATUS       (CLOCKS_BASE + 0xa4u)
#define FC0_RESULT       (CLOCKS_BASE + 0xa8u)
#define FC0_SRC_CLK_SYS  0x09u
#define FC0_STATUS_RUNNING (1u << 8)
#define FC0_STATUS_DONE    (1u << 4)
#define FC0_RESULT_KHZ_S  5

/* UART0 is a PrimeCell PL011, same layout as the ARM port. */
#define RP_UART_DR    (UART0_BASE + 0x00u)
#define RP_UART_FR    (UART0_BASE + 0x18u)
#define RP_UART_IBRD  (UART0_BASE + 0x24u)
#define RP_UART_FBRD  (UART0_BASE + 0x28u)
#define RP_UART_LCR_H (UART0_BASE + 0x2cu)
#define RP_UART_CR    (UART0_BASE + 0x30u)
#define RP_UART_ICR   (UART0_BASE + 0x44u)

#define RP_UART_FR_TXFF (1u << 5)
#define RP_UART_FR_RXFE (1u << 4)
#define RP_UART_FR_BUSY (1u << 3)

#define RP_UART_LCR_H_FEN  (1u << 4)
#define RP_UART_LCR_H_WLEN (3u << 5)

#define RP_UART_CR_UARTEN (1u << 0)
#define RP_UART_CR_TXE    (1u << 8)
#define RP_UART_CR_RXE    (1u << 9)

/* Console pins: GPIO0 = UART0 TX, GPIO1 = RX (FUNCSEL 2).  Every RP2350
 * pad comes out of reset with ISO set, so it must be cleared explicitly. */
#define RP_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define RP_GPIO_PAD(n)  (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))
#define RP_GPIO_FUNC_UART 0x02u
#define RP_GPIO_FUNC_SIO  0x05u
#define RP_PAD_OD         (1u << 7)
#define RP_PAD_IE         (1u << 6)
#define RP_PAD_ISO        (1u << 8)

/* SIO GPIO.  RP2350 words are 8 bytes apart (64-bit GPIO for the B part),
 * but the low 32 bits still cover GPIO0..31. */
#define SIO_GPIO_OUT_CLR (SIO_BASE + 0x020u)
#define SIO_GPIO_OUT_SET (SIO_BASE + 0x018u)
#define SIO_GPIO_OE_SET  (SIO_BASE + 0x038u)

/* ---- peripherals used by the device port ------------------------------ */

#define ADC_BASE        0x400a0000u
#define SPI0_BASE       0x40080000u
#define SPI1_BASE       0x40088000u
#define TIMER0_BASE     0x400b0000u
#define TIMERAWL_OFFSET 0x00000028u

/* RESETS bits for the blocks the panel/ADC bring up. */
#define RESET_ADC       (1u << 0)
#define RESET_DMA       (1u << 2)
#define RESET_SPI0      (1u << 18)
#define RESET_SPI1      (1u << 19)

/* clk_adc sits after clk_hstx; its AUXSRC has no clk_sys option, so the
 * ADC runs from PLL_SYS / 4 = 37.5 MHz at the 150 MHz boot rate. */
#define CLK_ADC_CTRL_OFFSET     0x6cu
#define CLK_ADC_DIV_OFFSET      0x70u
#define CLK_ADC_SELECTED_OFFSET 0x74u
#define CLK_ADC_CTRL_ENABLE     (1u << 11)
#define CLK_ADC_CTRL_AUXSRC_PLL_SYS 0x1u

/* SIO GPIO input and output-enable clear. */
#define SIO_GPIO_IN      (SIO_BASE + 0x004u)
#define SIO_GPIO_OE_CLR  (SIO_BASE + 0x040u)

#define RP_GPIO_FUNC_SPI  0x01u
#define RP_GPIO_FUNC_NULL 0x1fu

/* SPI (PL022), same register layout as the ARM port. */
#define RP_SPI_CR0    (0x00u)
#define RP_SPI_CR1    (0x04u)
#define RP_SPI_DR     (0x08u)
#define RP_SPI_SR     (0x0cu)
#define RP_SPI_CPSR   (0x10u)
#define RP_SPI_ICR    (0x14u)
#define RP_SPI_DMACR  (0x24u)

#define RP_SPI_SR_TFE (1u << 0)
#define RP_SPI_SR_TNF (1u << 1)
#define RP_SPI_SR_RNE (1u << 2)
#define RP_SPI_SR_RFF (1u << 3)
#define RP_SPI_SR_BSY (1u << 4)

#define RP_SPI_CR0_DSS_8  (7u << 0)
#define RP_SPI_CR1_SSE    (1u << 1)
#define RP_SPI_ICR_ROR    (1u << 0)
#define RP_SPI_DMACR_RXDMAE (1u << 0)
#define RP_SPI_DMACR_TXDMAE (1u << 1)

/* DMA: sixteen channels, 0x40 bytes apart. */
#define DMA_BASE             0x50000000u
#define DMA_CH_OFFSET(n)     ((n) * 0x40u)
#define DMA_CH_READ_ADDR     (0x00u)
#define DMA_CH_WRITE_ADDR    (0x04u)
#define DMA_CH_TRANS_COUNT   (0x08u)
#define DMA_CH_CTRL_TRIG     (0x0cu)

#define DMA_CTRL_EN          (1u << 0)
#define DMA_CTRL_DATA_BYTE   (0u << 2)
#define DMA_CTRL_INCR_READ   (1u << 4)
#define DMA_CTRL_TREQ(n)     ((n) << 17)
#define DMA_CTRL_BUSY        (1u << 26)
#define DREQ_SPI1_TX         26u

/* ATK-DNRP2350AM user LED: GPIO3, wired for sink current (LOW = ON). */
#define RP_LED_GPIO 3u

/* The ROM's flash helpers are called with a flash *offset*, not an XIP
 * address; littlefs starts at 0x80000 (see internal/build/uf2.go). */
#define RP_FLASH_SECTOR      4096u
#define RP_SECTOR_ERASE_CMD  0x20u

#endif
