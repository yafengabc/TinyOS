#include "keyboard.h"
#include "io.h"

#define KB_BUFSZ 256

static volatile uint8_t buf[KB_BUFSZ];
static volatile uint32_t head, tail;
static int shift, caps, extended;

static const char normal_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const char shift_map[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

void keyboard_init(void)
{
    head = tail = 0;
    shift = caps = extended = 0;
}

static void push(uint8_t c)
{
    uint32_t next = (head + 1) % KB_BUFSZ;
    if (next != tail) {
        buf[head] = c;
        head = next;
    }
}

void keyboard_irq(void)
{
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;
    }

    if (sc == 0x2A || sc == 0x36) {
        shift = 1;
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift = 0;
        return;
    }
    if (sc == 0x3A) {
        caps = !caps;
        return;
    }
    if (sc & 0x80)
        return;

    char c = 0;
    if (sc == 0x1C)
        c = '\n';
    else if (sc == 0x0E)
        c = '\b';
    else if (sc < 128) {
        c = shift ? shift_map[sc] : normal_map[sc];
        if (!c) {
            /* apply caps lock to letters */
            char base = normal_map[sc];
            if (base >= 'a' && base <= 'z') {
                c = (caps ^ shift) ? (char)(base - 32) : base;
            }
        }
    }
    if (c)
        push((uint8_t)c);
}

int keyboard_haschar(void)
{
    return head != tail;
}

int keyboard_getchar(void)
{
    if (head == tail)
        return -1;
    uint8_t c = buf[tail];
    tail = (tail + 1) % KB_BUFSZ;
    return c;
}
