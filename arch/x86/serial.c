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
    drain_rx();
}

void serial_putc(char c)
{
    /* Make the output look sane on a normal terminal. */
    if (c == '\n')
        serial_putc('\r');
    if (c == '\b') {
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
        return;
    }
    /* Output at 38400 baud takes ~0.26 ms per character.  QEMU does not
       throttle input to the baud rate, so a whole line can fill the 16
       byte RX FIFO while we are printing an echo.  Keep draining it. */
    while (!(inb(COM1 + 5) & 0x20))
        drain_rx();
    outb(COM1, (uint8_t)c);
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
