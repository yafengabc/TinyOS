#ifndef TINYOS_RP2040_MEMMAP_H
#define TINYOS_RP2040_MEMMAP_H

#include "types.h"
#include "scs.h"

/* Memory map and clock constants for the Raspberry Pi RP2040 (Cortex-M0+). */

/* XIP flash (executed in place) and on-chip SRAM. */
#define XIP_BASE        0x10000000u
#define SRAM_BASE       0x20000000u

/* Boot ROM: the flash API function table and its lookup live at these
 * fixed offsets inside the (always-mapped) boot ROM at 0x00000000. */
#define BOOTROM_FUNC_TABLE_OFFSET    0x14u
#define BOOTROM_TABLE_LOOKUP_OFFSET 0x18u

/* Peripheral bases (from the RP2040 address map). */
#define XOSC_BASE       0x40024000u
#define CLOCKS_BASE     0x40008000u
#define RESETS_BASE     0x4000c000u
#define IO_BANK0_BASE   0x40014000u
#define PADS_BANK0_BASE 0x4001c000u
#define UART0_BASE      0x40034000u
#define UART1_BASE      0x40038000u
#define SPI0_BASE       0x4003c000u
#define SPI1_BASE       0x40040000u
#define I2C0_BASE       0x40044000u
#define I2C1_BASE       0x40048000u
#define DMA_BASE        0x50000000u
#define PLL_SYS_BASE    0x40028000u
#define PLL_USB_BASE    0x4002c000u
#define SIO_BASE        0xd0000000u
#define SIO_CPUID       (SIO_BASE + 0x000u)  /* 0 on core 0, 1 on core 1 */

/* RP2040 boot ROM flash API function codes (ROM_TABLE_CODE('R','E') etc.). */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u
#define ROM_FUNC_FLASH_RANGE_PROGRAM 0x5052u
#define ROM_FUNC_FLASH_FLUSH_CACHE   0x4346u
#define ROM_FUNC_CONNECT_INTERNAL_FLASH 0x4649u  /* 'I','F' */
#define ROM_FUNC_FLASH_EXIT_XIP      0x5845u    /* 'E','X' */
#define ROM_FUNC_FLASH_ENTER_CMD_XIP 0x5843u    /* 'C','X' */

/* RESETS block: register offsets and the bits we release.  The reset
 * register is active-high (1 = in reset); de-assert by clearing a bit. */
#define RESETS_RESET_OFFSET    0x00u
#define RESETS_RESET_DONE_OFFSET 0x08u
#define RESET_UART0   (1u << 22)
#define RESET_UART1   (1u << 23)
#define RESET_SPI0    (1u << 16)
#define RESET_SPI1    (1u << 17)
#define RESET_I2C0    (1u << 3)
#define RESET_I2C1    (1u << 4)
#define RESET_DMA     (1u << 2)
#define RESET_IO_BANK0 (1u << 5)
#define RESET_PADS_BANK0 (1u << 8)
#define RESET_PLL_SYS (1u << 12)   /* bit 14 is PWM, not PLL_SYS */
#define RESET_PWM     (1u << 14)
#define RESET_ADC     (1u << 0)

/* ---- Clocks -------------------------------------------------------------
 * A normal flash boot leaves XOSC DISABLED: clk_ref (and therefore clk_sys,
 * whose reset SRC is clk_ref) runs from ROSC.  Measured on the bench, the
 * UART divider that should give 115200 baud gave ~50000 instead, i.e. a
 * ~5 MHz clock, not 12 MHz.  Worse, PLL_USB takes its reference from the
 * crystal, so its 48 MHz VCO can never lock and the CDC port never
 * enumerates.  plat_board_early() enables XOSC and repoints clk_ref at it,
 * then starts PLL_SYS at 125 MHz and moves clk_sys onto it before anything
 * programs a divisor.  Pico-sdk and MicroPython both run the RP2040 at
 * 125 MHz; at 12 MHz the USB control-transfer turnaround was too slow for
 * the host, which reset the port right after the first device descriptor. */
#define XOSC_CTRL               (XOSC_BASE + 0x00u)
#define XOSC_STATUS             (XOSC_BASE + 0x04u)
#define XOSC_CTRL_ENABLE_12M    ((0xfabu << 12) | 0xaa0u)
#define XOSC_STATUS_STABLE      (1u << 31)

#define CLK_REF_CTRL_OFFSET      0x30u
#define CLK_REF_SELECTED_OFFSET  0x38u
#define CLK_SYS_CTRL_OFFSET      0x3cu
#define CLK_SYS_DIV_OFFSET       0x40u
#define CLK_SYS_SELECTED_OFFSET  0x44u
#define CLK_PERI_CTRL_OFFSET     0x48u
#define CLK_PERI_DIV_OFFSET      0x4cu
#define CLK_REF_CTRL_SRC_XOSC    0x2u
#define CLK_SYS_CTRL_SRC_CLKREF  0x0u
#define CLK_SYS_CTRL_SRC_AUX     0x1u
#define CLK_SYS_CTRL_AUXSRC_PLL_SYS 0x0u
#define CLK_PERI_CTRL_ENABLE     (1u << 11)
#define CLK_PERI_CTRL_AUXSRC_CLK_SYS 0x0u
/* RP2040 clock dividers are 24.8 fixed point (INT in bits 31:8, FRAC in
 * bits 7:0) - pico-sdk's clock_configure_undivided() writes 1u << 8.  The
 * value 0x00010000 is the RP2350/16.16 form and means DIV=256 here: it
 * drops clk_sys from 12 MHz to ~47 kHz and the UART from 115200 to ~450
 * baud, which looks exactly like a completely dead board. */
#define CLK_DIV_1                0x00000100u   /* 24.8 fixed point: 1.0  */

/* PLL_SYS: 12 MHz XOSC / 1 * 125 = 1500 MHz VCO, /6/2 = 125 MHz.  The same
 * FBDIV/posts as pico-sdk's set_sys_clock_khz(125000). */
#define PLL_SYS_CS              (PLL_SYS_BASE + 0x00u)
#define PLL_SYS_PWR             (PLL_SYS_BASE + 0x04u)
#define PLL_SYS_FBDIV           (PLL_SYS_BASE + 0x08u)
#define PLL_SYS_PRIM            (PLL_SYS_BASE + 0x0cu)
#define PLL_CS_LOCK             0x80000000u
#define RP2040_SYS_CLK_HZ       125000000u

/* SysTick: present on every Cortex-M, driven by clk_sys.  After
 * clocks_init() that is PLL_SYS at 125 MHz - not the ROM's ROSC and not the
 * 48 MHz USB PLL. */
#define ARM_SYSTICK_CSR  (ARM_SCS_BASE + 0x0010u)
#define ARM_SYSTICK_LOAD (ARM_SCS_BASE + 0x0014u)
#define ARM_SYSTICK_VAL  (ARM_SCS_BASE + 0x0018u)
#define ARM_SYSTICK_CAL  (ARM_SCS_BASE + 0x001Cu)
#define ARM_SYSTICK_CTRL ARM_SYSTICK_CSR

#define ARM_SYSTICK_CLK  RP2040_SYS_CLK_HZ

/* Cortex-M0+ vector table offset register.  The stage-2 leaves VTOR pointing
 * at the boot ROM (0x00000000); we must repoint it at our own vector table at
 * 0x10000100 or every exception would land in the boot ROM's handlers.  M0+
 * requires the table aligned to 256 bytes (0x10000100 satisfies this). */
#define ARM_VTOR        (ARM_SCS_BASE + 0xED08u)

/* UART0 is a PrimeCell PL011. */
#define RP_UART_DR    (UART0_BASE + 0x00u)
#define RP_UART_FR    (UART0_BASE + 0x18u)
#define RP_UART_IBRD  (UART0_BASE + 0x24u)
#define RP_UART_FBRD  (UART0_BASE + 0x28u)
#define RP_UART_LCR_H (UART0_BASE + 0x2cu)
#define RP_UART_CR    (UART0_BASE + 0x30u)

#define RP_UART_FR_TXFF (1u << 5)
#define RP_UART_FR_RXFE (1u << 4)
#define RP_UART_FR_BUSY (1u << 3)
#define RP_UART_FR_TXFE (1u << 7)

#define RP_UART_LCR_H_FEN  (1u << 4)
#define RP_UART_LCR_H_WLEN (3u << 5)

#define RP_UART_CR_UARTEN (1u << 0)
#define RP_UART_CR_TXE    (1u << 8)
#define RP_UART_CR_RXE    (1u << 9)

/* On the Pico the console is UART0 on GPIO0 (TX) / GPIO1 (RX). */
#define RP_GPIO0_CTRL_OFFSET 0x04u
#define RP_GPIO1_CTRL_OFFSET 0x0cu
#define RP_GPIO0_PAD_OFFSET  0x04u
#define RP_GPIO1_PAD_OFFSET  0x08u
#define RP_GPIO_FUNC_UART    0x02u   /* FUNCSEL_UART0_TX / _RX   */

#endif
