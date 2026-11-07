#ifndef TINYOS_ARCH_SCS_H
#define TINYOS_ARCH_SCS_H

#include "types.h"

/* ARMv7-M System Control Space (SCS): the core's own registers, present on
 * every Cortex-M.  This is architecture, not board, so the map lives here
 * and both the generic layer and the boards may include it.
 *
 * The SCS holds the SysTick timer, the NVIC and the exception priority
 * registers.  Board maps (arm_memmap.h) add their peripherals on top. */

#define ARM_SCS_BASE     0xE000E000u

/* Exception priorities.  Each system handler owns one byte: SHPR2 puts
 * SVCall in [31:24], SHPR3 puts SysTick in [31:24] and PendSV in [23:16].
 * Cortex-M3 implements the top three bits, so values step by 0x20 from
 * 0x00 (highest) to 0xE0 (lowest); writing a value that is not a multiple
 * of 0x20 simply loses the unimplemented low bits. */
#define ARM_ICSR         (ARM_SCS_BASE + 0xD04u)
/* Set when an SVC was pended instead of taken (a higher-priority exception
 * was pending at the same instruction boundary).  QEMU/mps2 does not set
 * this bit for a priority-pended SVC; it shows up in VECTPENDING instead
 * (measured: ICSR=0x0000B80F while SysTick 15 is active and SVC 11 is
 * pending), so both are tested. */
#define ARM_ICSR_PENDSVSET     (1u << 28)
#define ARM_ICSR_VECTPENDING   (0x1FFu << 12)

#define ARM_SHPR1        (ARM_SCS_BASE + 0xD18u)
#define ARM_SHPR2        (ARM_SCS_BASE + 0xD1Cu)
#define ARM_SHPR3        (ARM_SCS_BASE + 0xD20u)

/* SysTick outranks SVCall: a task blocked inside its syscall idles with
 * SVCall active, and only a higher-priority exception can preempt it.
 * PendSV stays at the bottom for future deferred context switches.
 *
 * ARMv6-M (Cortex-M0+) has no SHPR1: its only fault is HardFault, whose
 * priority is fixed at -1, so the configurable fault priorities are M3
 * and up.  SHPR1 packs UsageFault in [31:24], BusFault in [23:16] and
 * MemManage in [15:8]. */
#define ARM_PRIO_SYSTICK 0x00u
#define ARM_PRIO_SVC     0x40u
#define ARM_PRIO_PENDSV  0xE0u
#define ARM_PRIO_FAULT   0x40u

/* ---- NVIC ------------------------------------------------------------- */

/* Nested Vectored Interrupt Controller.  ISER/ICER are the per-IRQ enable
 * bits, one 32-bit word per group of 32 IRQs; IPR holds one priority byte
 * per IRQ (at most 8 bits wide, so the low bits may read back as zero). */
#define ARM_NVIC_ISER(n)  (ARM_SCS_BASE + 0x100u + 4u * (n))
#define ARM_NVIC_ICER(n)  (ARM_SCS_BASE + 0x180u + 4u * (n))
#define ARM_NVIC_ISPR(n)  (ARM_SCS_BASE + 0x200u + 4u * (n))
#define ARM_NVIC_IPR(irq) (ARM_SCS_BASE + 0x400u + (uint32_t)(irq))

/* ---- reset ------------------------------------------------------------ */

/* Application Interrupt and Reset Control.  Every write needs the key in
 * [31:16] (any other value is ignored, which is how a typo turns a reset
 * into nothing at all), and SYSRESETREQ in [2] asks the core to reset the
 * system.  Present on ARMv6-M and ARMv7-M alike. */
#define ARM_AIRCR             (ARM_SCS_BASE + 0xD0Cu)
#define ARM_AIRCR_VECTKEY     0x05FA0000u
#define ARM_AIRCR_SYSRESETREQ (1u << 2)

/* ---- fault status ----------------------------------------------------- */

/* System handler control: the configurable faults are off at reset, which
 * escalates MemManage/BusFault/UsageFault to HardFault and hides why the
 * task died.  The port turns them on so `prot` and `crash` report the real
 * exception name. */
#define ARM_SHCSR        (ARM_SCS_BASE + 0xD24u)
#define ARM_SHCSR_MEMFAULTENA (1u << 16)
#define ARM_SHCSR_BUSFAULTENA (1u << 17)
#define ARM_SHCSR_USGFAULTENA (1u << 18)

#define ARM_CFSR         (ARM_SCS_BASE + 0xD28u)
#define ARM_HFSR         (ARM_SCS_BASE + 0xD2Cu)
#define ARM_MMFAR        (ARM_SCS_BASE + 0xD34u)
#define ARM_BFAR         (ARM_SCS_BASE + 0xD38u)

/* CFSR validity bits: the address registers only mean something when set. */
#define ARM_CFSR_MMARVALID (1u << 7)
#define ARM_CFSR_BFARVALID (1u << 15)
/* MemManage bits that say the *exception frame* could not be written, which
 * is how a stack overrun announces itself: the push of r0-r3/lr/pc is the
 * first access to fail, so the reported pc is meaningless and SP has already
 * left the stack. */
#define ARM_CFSR_MSTKERR   (1u << 4)
#define ARM_CFSR_MUNSTKERR (1u << 3)

/* ARMv8-M (Cortex-M33) TrustZone diagnostics.  A security violation is
 * escalated to HardFault with HFSR.FORCED set and CFSR completely zero -
 * without SFSR there is no way to tell it from an ordinary bus fault.
 * These are Secure-world registers; in Non-secure state they read as zero. */
#define ARM_SAU_CTRL     (ARM_SCS_BASE + 0xDD0u)
#define ARM_SAU_TYPE     (ARM_SCS_BASE + 0xDD4u)
#define ARM_SFSR         (ARM_SCS_BASE + 0xDE4u)
#define ARM_SFAR         (ARM_SCS_BASE + 0xDE8u)

/* ---- MPU -------------------------------------------------------------- */

/* ARMv7-M Memory Protection Unit.  Eight regions on the Cortex-M3 (probe
 * MPU_TYPE.DREGION if a board ever uses a different core).  A region is a
 * naturally aligned power-of-two block; there is no base/limit form, which
 * is why a task window is described as a few aligned blocks rather than a
 * byte-exact range (see plat_task_protect in platform.c). */
#define ARM_MPU_TYPE     (ARM_SCS_BASE + 0xD90u)
#define ARM_MPU_CTRL     (ARM_SCS_BASE + 0xD94u)
#define ARM_MPU_RNR      (ARM_SCS_BASE + 0xD98u)
#define ARM_MPU_RBAR     (ARM_SCS_BASE + 0xD9Cu)
#define ARM_MPU_RASR     (ARM_SCS_BASE + 0xDA0u)   /* ARMv7-M only */

/* ARMv8-M PMSAv8 reuses the same two slots with a different meaning:
 * RBAR holds base[31:5] | shareability | AP | XN, and the register at 0xDA0
 * becomes RLAR - the *inclusive limit* plus the attribute index and the
 * enable bit, instead of a size field.  Region numbering, priority (higher
 * number wins on overlap) and the 32-byte minimum are unchanged, which is
 * why only the writer differs between the two ports. */
#define ARM_MPU_RLAR     (ARM_SCS_BASE + 0xDA0u)
#define ARM_MPU_MAIR0    (ARM_SCS_BASE + 0xDC0u)
#define ARM_MPU_MAIR1    (ARM_SCS_BASE + 0xDC4u)

#define ARM_MPU_CTRL_ENABLE     (1u << 0)
/* PRIVDEFENA: privileged code keeps the default map, so the kernel reads
 * and writes every byte of RAM and the MMIO without a region per device.
 * Unprivileged accesses to the default map are denied. */
#define ARM_MPU_CTRL_PRIVDEFENA (1u << 2)

/* Smallest region an ARMv7-M MPU can describe (RASR SIZE field 4). */
#define ARM_MPU_MIN_SIZE    32u

/* RASR fields (ARMv7-M ARM B3.5.10). */
#define ARM_MPU_XN          (1u << 28)   /* execute-never                  */
#define ARM_MPU_AP_PRIV_RW  (1u << 24)   /* 0b001 privileged RW, user none */
#define ARM_MPU_AP_FULL     (3u << 24)   /* 0b011 privileged + user RW     */
#define ARM_MPU_AP_RO       (6u << 24)   /* 0b110 read-only, both worlds   */
/* Normal, non-shareable... mps2 SRAM is shareable write-back: S=1 C=1 B=1. */
#define ARM_MPU_SRAM        ((1u << 18) | (1u << 17) | (1u << 16))

#endif
