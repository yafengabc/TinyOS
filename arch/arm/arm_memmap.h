#ifndef TINYOS_ARM_MEMMAP_H
#define TINYOS_ARM_MEMMAP_H

#include "types.h"
#include "scs.h"

/* Memory map for QEMU mps2-an385 (Cortex-M3).  The core's System Control
 * Space (SCS base, exception priorities) comes from scs.h. */

/* CMSDK APB UART 0 */
#define ARM_UART0_BASE   0x40004000u
#define ARM_UART0_DATA   (ARM_UART0_BASE + 0x00u)
#define ARM_UART0_STATE  (ARM_UART0_BASE + 0x04u)
#define ARM_UART0_CTRL   (ARM_UART0_BASE + 0x08u)
#define ARM_UART0_BAUD   (ARM_UART0_BASE + 0x10u)

/* SysTick is present on every Cortex-M and is driven by the 25 MHz
 * reference clock on this board. */
#define ARM_SYSTICK_CSR  (ARM_SCS_BASE + 0x0010u)   /* CTRL   */
#define ARM_SYSTICK_LOAD (ARM_SCS_BASE + 0x0014u)   /* RELOAD */
#define ARM_SYSTICK_VAL  (ARM_SCS_BASE + 0x0018u)   /* VALUE  */
#define ARM_SYSTICK_CAL  (ARM_SCS_BASE + 0x001Cu)   /* CALIB  */

#define ARM_SYSTICK_CTRL ARM_SYSTICK_CSR

#define ARM_SYSTICK_CLK  25000000u

/* mps2-an385: the CMSDK dual timer sits at 0x40002000. */
#define ARM_DUALTIMER_BASE 0x40002000u

#endif
