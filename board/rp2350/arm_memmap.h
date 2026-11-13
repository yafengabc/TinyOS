#ifndef TINYOS_RP2350_MEMMAP_H
#define TINYOS_RP2350_MEMMAP_H

#include "types.h"
#include "scs.h"

/* Memory map and clock constants for the Raspberry Pi RP2350
 * (dual Cortex-M33, ARMv8-M with TrustZone, running in Secure state).
 *
 * Every peripheral moved between RP2040 and RP2350 except CLOCKS, RESETS,
 * XIP and SIO.  The numbers below are taken from the pico-sdk register
 * headers (rp2350/hardware_regs/include/hardware/regs/addressmap.h), not
 * from the older RP2040 datasheet. */

#define XIP_BASE        0x10000000u
#define SRAM_BASE       0x20000000u
#define SRAM_END        0x20082000u   /* 520 KiB, striped SRAM0..7 + 2x4K */

/* Boot ROM well-known pointers.  On RP2350 the ARM lookup routine is a
 * 16-bit pointer stored at 0x16, and it takes (code, flags) rather than
 * the RP2040 (table, code) pair.  RT_FLAG_FUNC_ARM_SEC = 0x0004. */
#define BOOTROM_LOOKUP_PTR  0x16u
#define RT_FLAG_FUNC_ARM_SEC 0x0004u

#define XOSC_BASE       0x40048000u
#define CLOCKS_BASE     0x40010000u
#define RESETS_BASE     0x40020000u
#define IO_BANK0_BASE   0x40028000u
#define PADS_BANK0_BASE 0x40038000u
#define UART0_BASE      0x40070000u
#define UART1_BASE      0x40078000u
#define SPI0_BASE       0x40080000u
#define SPI1_BASE       0x40088000u
#define PLL_SYS_BASE    0x40050000u
#define ADC_BASE        0x400a0000u
#define SIO_BASE        0xd0000000u
#define SIO_CPUID       (SIO_BASE + 0x000u)

/* RESETS: active-high reset bits, de-assert by clearing.  The bit
 * positions are NOT the RP2040 ones. */
#define RESETS_RESET_OFFSET      0x00u
#define RESETS_RESET_DONE_OFFSET 0x08u
#define RESET_UART0     (1u << 26)
#define RESET_UART1     (1u << 27)
#define RESET_PADS_BANK0 (1u << 9)
#define RESET_IO_BANK0  (1u << 6)
#define RESET_SPI0      (1u << 18)
#define RESET_SPI1      (1u << 19)
/* PLL_SYS is bit 14 on the RP2350 (PLL_USB is 15 - see usb_cdc.c). */
#define RESET_PLL_SYS   (1u << 14)
/* DMA sits on bit 2 here (RP2040 used bit 1). */
#define RESET_DMA       (1u << 2)
/* The ADC block resets asserted and unclocked, which is why every conversion
 * used to time out.  Bit 0, same as the RP2040. */
#define RESET_ADC       (1u << 0)

/* CLOCKS.  plat_board_early() enables the 12 MHz crystal, pins clk_ref to
 * it, brings PLL_SYS up to 150 MHz and switches clk_sys onto it.  clk_peri
 * follows clk_sys, so the CPU, SysTick, UART and SPI all run from one
 * measured 150 MHz (see pll_sys_init() in platform.c).  If the PLL never
 * locks the boot falls back to the ROM's clk_sys (about 12 MHz) and the
 * frequency counter + re-divided UART keep working - nothing else in the
 * port hard-codes a rate, every divisor comes from the measured g_clk_hz.
 *
 * XOSC_CTRL_ENABLE resets to DISABLE: "On power-up this field is
 * initialised to DISABLE and the chip runs from the ROSC" (rp2350 xosc.h).
 * A flash boot does NOT necessarily leave the crystal running - measured on
 * hardware: XOSC_STATUS=0, CLK_REF_CTRL=0, CLK_REF_SELECTED=1, i.e. the
 * kernel was running from the ~12 MHz ROSC.  ROSC is close enough to 12 MHz
 * for the UART and FC0, but PLL_USB takes its reference from the crystal, so
 * without this enable the VCO powers up and never locks (usb stage=2).
 * ENABLE = 0xfab << 12, FREQ_RANGE = 0xaa0 (1-15 MHz) << 0. */
#define XOSC_CTRL               (XOSC_BASE + 0x00u)
#define XOSC_STATUS             (XOSC_BASE + 0x04u)
#define XOSC_CTRL_ENABLE_12M    ((0xfabu << 12) | 0xaa0u)
#define XOSC_STATUS_STABLE      (1u << 31)

#define CLK_REF_CTRL_OFFSET     0x30u
#define CLK_REF_SELECTED_OFFSET 0x38u
#define CLK_SYS_CTRL_OFFSET     0x3cu
#define CLK_SYS_DIV_OFFSET      0x40u
#define CLK_SYS_SELECTED_OFFSET 0x44u
#define CLK_PERI_CTRL_OFFSET   0x48u
#define CLK_PERI_DIV_OFFSET     0x4cu
#define CLK_PERI_SELECTED_OFFSET 0x50u
/* clk_adc sits after clk_hstx, not after clk_peri: offsets 0x6c/0x70/0x74.
 * Its AUXSRC has no clk_sys option (pll_usb, pll_sys, rosc, xosc, gpin0/1),
 * so the ADC runs from PLL_SYS with an integer divider - 150/4 = 37.5 MHz,
 * inside the 48 MHz limit. */
#define CLK_ADC_CTRL_OFFSET     0x6cu
#define CLK_ADC_DIV_OFFSET      0x70u
#define CLK_ADC_SELECTED_OFFSET 0x74u
#define CLK_ADC_CTRL_ENABLE     (1u << 11)
#define CLK_ADC_CTRL_AUXSRC_PLL_SYS 0x1u

#define CLK_REF_CTRL_SRC_XOSC   0x2u
/* CLK_SYS_CTRL: SRC bit [0] (0 = clk_ref, 1 = the AUX source selected in
 * bits [7:5]); AUXSRC 0 is clksrc_pll_sys, 2 is ROSC (the reset value). */
#define CLK_SYS_CTRL_SRC_CLKREF 0x0u
#define CLK_SYS_CTRL_SRC_AUX    0x1u
#define CLK_SYS_CTRL_AUX_PLL_SYS 0x0u
#define CLK_PERI_CTRL_ENABLE    (1u << 11)
#define CLK_PERI_CTRL_AUXSRC_CLK_SYS 0x0u
#define CLK_DIV_1               0x00010000u   /* 16.16 fixed point: 1.0 */

/* PLL_SYS at 0x40050000 has exactly the register layout of PLL_USB, which
 * usb_cdc.c already drives for the 48 MHz clk_usb: CS/PWR/FBDIV/PRIM at
 * 0x00/04/08/0c, LOCK = CS bit 31, REFDIV = CS[5:0], POSTDIV1 = PRIM[18:16],
 * POSTDIV2 = PRIM[14:12].  VCO range is 750-1600 MHz; the pico-sdk default
 * for a 12 MHz crystal is REFDIV 1, FBDIV 125 (1500 MHz VCO), postdiv
 * 5 and 2 = 150 MHz - pll_sys_init() uses exactly that. */
#define PLL_SYS_BASE            0x40050000u
#define PLL_SYS_CS(off)         (*(volatile uint32_t *)(PLL_SYS_BASE + (off)))
#define PLL_SYS_CS_REG          PLL_SYS_CS(0x00)
#define PLL_SYS_PWR_REG         PLL_SYS_CS(0x04)
#define PLL_SYS_FBDIV_REG       PLL_SYS_CS(0x08)
#define PLL_SYS_PRIM_REG        PLL_SYS_CS(0x0c)
#define PLL_SYS_CS_LOCK         0x80000000u
#define PLL_SYS_PWR_PD          0x00000001u
#define PLL_SYS_PWR_VCOPD       0x00000020u
#define PLL_SYS_PWR_POSTDIVPD   0x00000008u

/* Frequency counter: measures clk_sys against clk_ref (12 MHz).  Used only
 * as a boot diagnostic, never to derive the baud rate. */
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

#define ARM_SYSTICK_CSR  (ARM_SCS_BASE + 0x0010u)
#define ARM_SYSTICK_LOAD (ARM_SCS_BASE + 0x0014u)
#define ARM_SYSTICK_VAL  (ARM_SCS_BASE + 0x0018u)
#define ARM_SYSTICK_CAL  (ARM_SCS_BASE + 0x001Cu)
#define ARM_SYSTICK_CTRL ARM_SYSTICK_CSR

#define ARM_SYSTICK_CLK  12000000u

/* VTOR_S (Secure vector table offset).  ARM_SCS_BASE is 0xE000E000, so
 * the SCB registers all sit at +0xDxx (see scs.h): VTOR_S is 0xE000ED08,
 * i.e. +0xD08.  The old +0xED08 resolved to 0xE001CD08, an unimplemented
 * PPB address - writing it faults on Cortex-M33.  ARMv8-M also wants the
 * table 128-byte aligned, which the flash origin satisfies. */
#define ARM_VTOR        (ARM_SCS_BASE + 0x0D08u)

/* UART0 is a PrimeCell PL011, exactly the RP2040 register layout. */
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

/* Console is UART0 on GPIO0 (TX) / GPIO1 (RX), the same pins the Pico SDK
 * uses for stdio_uart and the pins a USB-TTL adapter is normally wired to. */
#define RP_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define RP_GPIO_PAD(n)  (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))
#define RP_GPIO_FUNC_SPI  0x01u    /* FUNCSEL_SPI: SPI0 or SPI1, per pin  */
#define RP_GPIO_FUNC_UART 0x02u    /* FUNCSEL_UART0_TX / _RX */
#define RP_GPIO_FUNC_SIO  0x05u
/* FUNCSEL "NULL": the pin belongs to no peripheral.  An ADC input needs this
 * (or any non-digital function) plus a pad with the digital input off. */
#define RP_GPIO_FUNC_NULL 0x1fu

/* PIO on the RP2350: three blocks of four state machines, 1 MiB apart
 * (0x50200000 / 0x50300000 / 0x50400000).  RESETS bits are 11/12/13 and
 * FUNCSEL 6/7/8.  Contrast with the RP2040, where PIO0/PIO1 sit on RESETS
 * bits 10/11 - a shared driver reads both from here. */
#define RP_PIO_BLOCKS       3
#define RP_PIO_BASE(b)      (0x50200000u + 0x100000u * (uint32_t)(b))
#define RP_PIO_RESET_BIT(b) (1u << (11 + (b)))
#define RP_PIO_FUNCSEL(b)   (6u + (uint32_t)(b))
#define RP_PAD_OD         (1u << 7)
#define RP_PAD_IE         (1u << 6)
/* RP2350-only: every PADS_BANK0 GPIO comes out of reset with ISO=1, which
 * electrically isolates the pin.  RP2040 has no such bit, so the port that
 * was copied from it left every pin deaf and mute.  pico-sdk's
 * gpio_set_function() clears ISO after programming FUNCSEL; our code must
 * do the same for every pad it uses (see the SDK register header:
 * PADS_BANK0_GPIO0_ISO_RESET = 1, bits = 0x100). */
#define RP_PAD_ISO        (1u << 8)

/* RP2350 SIO registers are 8 bytes apart (64-bit GPIO words for the 48-pin
 * RP2350B), not 4 as on the RP2040. */
#define SIO_GPIO_IN      (SIO_BASE + 0x004u)
#define SIO_GPIO_OUT     (SIO_BASE + 0x010u)
#define SIO_GPIO_OUT_SET (SIO_BASE + 0x018u)
#define SIO_GPIO_OUT_CLR (SIO_BASE + 0x020u)
#define SIO_GPIO_OE      (SIO_BASE + 0x030u)
#define SIO_GPIO_OE_SET  (SIO_BASE + 0x038u)
#define SIO_GPIO_OE_CLR  (SIO_BASE + 0x040u)

/* SPI (PL022).  The block kept its register layout from the RP2040; only
 * the base address moved.  Frame size is DSS+1 bits, so 8-bit frames are
 * DSS = 7.  Serial clock = clk_peri / (CPSDVSR * (1 + SCR)). */
#define RP_SPI_CR0    (0x00u)
#define RP_SPI_CR1    (0x04u)
#define RP_SPI_DR     (0x08u)
#define RP_SPI_SR     (0x0cu)
#define RP_SPI_CPSR   (0x10u)

#define RP_SPI_SR_TFE (1u << 0)
#define RP_SPI_SR_TNF (1u << 1)
#define RP_SPI_SR_RNE (1u << 2)
#define RP_SPI_SR_RFF (1u << 3)
#define RP_SPI_SR_BSY (1u << 4)

#define RP_SPI_CR0_DSS_8  (7u << 0)   /* 8-bit frames                      */
#define RP_SPI_CR1_SSE    (1u << 1)   /* enable the primecell              */
#define RP_SPI_ICR      (0x14u)
#define RP_SPI_ICR_ROR  (1u << 0)     /* clear the sticky RX overrun       */
#define RP_SPI_DMACR    (0x24u)
#define RP_SPI_DMACR_RXDMAE (1u << 0)
#define RP_SPI_DMACR_TXDMAE (1u << 1)

/* DMA.  Sixteen identical channels, 0x40 bytes apart, over one shared
 * register file at 0x50000000.  The four registers that matter are
 * READ_ADDR/WRITE_ADDR/TRANS_COUNT/CTRL_TRIG at +0/+4/+8/+0c; writing
 * CTRL_TRIG with EN arms and fires the transfer in one write.  RP2350
 * moved the fields around relative to the RP2040: TREQ_SEL is bits 22:17
 * (was 19:15) and BUSY is bit 26.  TRANS_COUNT is bits 27:0, bits 31:28
 * are a mode field that must stay zero for a normal transfer. */
#define DMA_BASE             0x50000000u
#define DMA_CH_OFFSET(n)     ((n) * 0x40u)
#define DMA_CH_READ_ADDR     (0x00u)
#define DMA_CH_WRITE_ADDR    (0x04u)
#define DMA_CH_TRANS_COUNT   (0x08u)
#define DMA_CH_CTRL_TRIG     (0x0cu)

#define DMA_CTRL_EN          (1u << 0)
#define DMA_CTRL_DATA_BYTE   (0u << 2)    /* 8-bit transfers                   */
#define DMA_CTRL_INCR_READ   (1u << 4)    /* walk the source buffer            */
#define DMA_CTRL_TREQ(n)     ((n) << 17)  /* pacing request selector          */
#define DREQ_SPI1_TX         26u          /* RP2350 numbering (RP2040: 18)    */
#define DMA_CTRL_BUSY        (1u << 26)

#endif
