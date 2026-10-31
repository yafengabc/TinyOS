#ifndef TINYOS_RV32_MEMMAP_H
#define TINYOS_RV32_MEMMAP_H

/* QEMU virt (rv32) memory map.  ESP32-C3 uses a different board header. */

#define RV32_RAM_BASE      0x80000000u
#define RV32_RAM_SIZE      0x08000000u   /* 128 MiB by default */

#define RV32_UART_BASE     0x10000000u
#define RV32_CLINT_BASE    0x02000000u

#define RV32_CLINT_MSIP      0x0000u
#define RV32_CLINT_MTIMECMP  0x4000u
#define RV32_CLINT_MTIMECMPH 0x4004u
#define RV32_CLINT_MTIME     0xBFF8u
#define RV32_CLINT_MTIMEH    0xBFFCu

/* mtime frequency on the virt machine. */
#define RV32_TIMER_HZ      10000000u

#endif
