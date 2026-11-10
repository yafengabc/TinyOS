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
#define PLL_SYS_BASE    0x40028000u
#define SIO_BASE        0xd0000000u
#define SIO_CPUID       (SIO_BASE + 0x000u)  /* 0 on core 0, 1 on core 1 */

/* RP2040 boot ROM flash API function codes (ROM_TABLE_CODE('R','E') etc.). */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u
#define ROM_FUNC_FLASH_RANGE_PROGRAM 0x5052u
#define ROM_FUNC_FLASH_FLUSH_CACHE   0x4346u

/* RESETS block: register offsets and the bits we release.  The reset
 * register is active-high (1 = in reset); de-assert by clearing a bit. */
#define RESETS_RESET_OFFSET    0x00u
#define RESETS_RESET_DONE_OFFSET 0x08u
#define RESET_UART0   (1u << 22)
#define RESET_IO_BANK0 (1u << 5)
#define RESET_PADS_BANK0 (1u << 8)

/* CLOCKS block: the clock control registers and the source / aux-source
 * mux values we use.  The stage-2 leaves clk_sys and clk_peri running from
 * the 12 MHz crystal (XOSC); we only re-assert clk_peri for the UART and
 * leave clk_sys alone (the SysTick rate below assumes the 12 MHz). */
#define CLK_REF_CTRL_OFFSET   0x30u
#define CLK_SYS_CTRL_OFFSET  0x3cu
#define CLK_PERI_CTRL_OFFSET 0x48u
#define CLK_REF_CTRL_SRC_XOSC    0x2u
#define CLK_SYS_CTRL_AUXSRC_XOSC 0x3u
#define CLK_PERI_CTRL_AUXSRC_XOSC 0x4u
#define CLK_PERI_CTRL_ENABLE  (1u << 11)

/* SysTick: present on every Cortex-M, driven by clk_sys (12 MHz). */
#define ARM_SYSTICK_CSR  (ARM_SCS_BASE + 0x0010u)
#define ARM_SYSTICK_LOAD (ARM_SCS_BASE + 0x0014u)
#define ARM_SYSTICK_VAL  (ARM_SCS_BASE + 0x0018u)
#define ARM_SYSTICK_CAL  (ARM_SCS_BASE + 0x001Cu)
#define ARM_SYSTICK_CTRL ARM_SYSTICK_CSR

#define ARM_SYSTICK_CLK  48000000u

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
#define RP_UART_FR_TXFE (1u << 3)

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
