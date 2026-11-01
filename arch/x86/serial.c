#include "serial.h"
#include "io.h"

#define RX_BUFSZ 256

/* The kernel drains the UART into this software buffer.  Polling only from
   the idle loop loses bytes when a whole line arrives at once (the 16550
   FIFO is just 16 bytes deep), which breaks paste and scripted input, so
   the readers call serial_poll() in a tight loop and the timer also drains
   it while a user program is running. */
static volatile uint8_t  rxbuf[RX_BUFSZ];
static volatile uint32_t rx_head, rx_tail;

/* End of output: set by serial_flush on the way down and never cleared.  The
 * timer keeps interrupting while we wait for the UART, and after "exit" the
 * shell is gone - a tick that wakes a sleeper would print on top of the
 * shutdown message. */
static volatile int shutting_down;

int serial_is_shutting_down(void)
{
    return shutting_down;
}

/* Push the last characters out before the machine stops.  QEMU's 16550
 * takes a simulated bit time per character, so the CPU is about to outrun
 * it: wait for the transmitter to go empty (TEMT, LSR bit 6).  The bound
 * matters - on real hardware or a stalled port, "shut down" must not become
 * "hang".
 *
 * This is a console-layer operation, not a device one the arch port has to
 * describe: other architectures in this tree have no such register to wait
 * on, and the kernel only calls it through plat_console_flush(). */
void serial_flush(void)
{
    uint32_t flags;
    uint32_t spin = 4000000;

    shutting_down = 1;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    while (!(inb(COM1 + 5) & 0x40)) {
        if (--spin == 0)
            break;
    }
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

void serial_init(void)
{
    rx_head = rx_tail = 0;
    outb(COM1 + 1, 0x00); /* no interrupts, we poll */
    outb(COM1 + 3, 0x80); /* enable DLAB */
    outb(COM1 + 0, 0x03); /* divisor low: 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0x07); /* enable + clear FIFO, 1 byte RX threshold */
    outb(COM1 + 4, 0x0B); /* RTS/DSR set */
}

static void drain_rx(void)
{
    /* Also called from the timer IRQ and from output waits.  Make sure the
       UART is not drained concurrently by an interrupt in the middle of a
       read (which would steal a byte and make us store a bogus one). */
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    while (inb(COM1 + 5) & 0x01) {
        uint8_t c = inb(COM1);
        uint32_t next = (rx_head + 1) % RX_BUFSZ;
        if (next != rx_tail) {
            rxbuf[rx_head] = c;
            rx_head = next;
        }
    }

    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

void serial_poll(void)
{
    if (shutting_down)
        return;             /* nobody is left to hand the bytes to */
    drain_rx();
}

/* One byte out, nothing added: output at 38400 baud takes ~0.26 ms per
   character, and QEMU does not throttle input to the baud rate, so a whole
   line can fill the 16 byte RX FIFO while we are printing an echo.  Keep
   draining it. */
static void serial_putc_raw(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        drain_rx();
    outb(COM1, (uint8_t)c);
}

void serial_putc(char c)
{
    /* Only CR before LF is a device detail.  Erasing is NOT: this used to
       expand '\b' into backspace-space-backspace by calling itself, which
       recursed until the kernel stack wrapped through zero and the next
       return jumped into nothing - one backspace was enough to wedge the
       machine.  The line discipline already writes the three characters it
       wants (kernel/tty.c), so a backspace simply goes out as one byte. */
    if (c == '\n')
        serial_putc_raw('\r');
    serial_putc_raw(c);
}

void serial_write(const char *s, size_t n)
{
    while (n--)
        serial_putc(*s++);
}

int serial_has_input(void)
{
    return rx_head != rx_tail;
}

int serial_read(void)
{
    while (!serial_has_input())
        ;
    uint8_t c = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUFSZ;
    return c;
}
