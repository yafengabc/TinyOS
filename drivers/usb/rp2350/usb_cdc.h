/* USB CDC-ACM console for the RP2350 (TinyUSB-backed; see usb_cdc.c).
 *
 * The public surface is deliberately small: the kernel console layer only
 * needs a byte sink, a byte source, and a "is the host actually listening"
 * flag.  Everything else here exists for the bring-up trace in platform.c. */

#include "types.h"

/* clk_hz is the measured clk_sys; the enumeration wait is scaled by it. */
void usb_cdc_console_init(uint32_t clk_hz);

/* Run TinyUSB's task loop.  Called every SysTick (1 ms) by the platform layer
 * and from the console API, because the host may enumerate at any time. */
void usb_cdc_poll(void);

/* Run TinyUSB's task loop from interrupt context, immediately after the
 * controller handler.  The shim's IRQ trampoline calls this; deferring the
 * task to the 1 ms tick is too slow for the host's control-transfer timing. */
void usb_cdc_irq_task(void);

/* Open the NVIC gate for IRQ 14.  dcd_init() has already registered the
 * controller handler; this is the second half of the handshake, called once
 * the console is otherwise up. */
void usb_cdc_irq_enable(void);

void usb_cdc_putc(char c);
/* Free space in the transmit FIFO: putc never blocks, it drops when full,
 * but knowing the room lets echo pace itself instead of losing characters. */
int  usb_cdc_tx_room(void);
void usb_cdc_flush(void);
int  usb_cdc_getchar(void);

/* Cross-core USB critical section (SIO hardware spinlock 30 + PRIMASK).
 * Everything that touches TinyUSB state must hold it; the shim's IRQ
 * trampoline wraps the controller handler with it too.  The returned word
 * is the saved PRIMASK and must be given back to usb_cdc_unlock(). */
uint32_t usb_cdc_lock(void);
void usb_cdc_unlock(uint32_t primask);

int  usb_cdc_configured(void);   /* 1 once the host has SET_CONFIGURATION */
int  usb_cdc_hw_ready(void);     /* 1 when reset + PLL handshake succeeded */
int  usb_cdc_init_stage(void);   /* 0 ok, 1 reset timeout, 2 PLL timeout  */
uint32_t usb_cdc_pll_boot(void); /* PLL_CS as the ROM handed it over      */

/* Force the host to see a disconnect/reconnect: on=0 drops the D+ pull-up,
 * on=1 puts it back.  Recovery path for a port the host has parked. */
void usb_cdc_replug(int on);

/* Bring-up probe, see usb_cdc_dbg() in usb_cdc.c for the index list. */
uint32_t usb_cdc_dbg(uint32_t which);
