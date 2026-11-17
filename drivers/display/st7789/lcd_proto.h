#ifndef TINYOS_LCD_PROTO_H
#define TINYOS_LCD_PROTO_H

#include "types.h"

/* ST7789 protocol layer shared by the RP2350 ARM and RV32 lowers (and, later,
 * any other board with this panel).
 *
 * The upper owns "what the controller is told": reset/sleep-out order, the
 * vendor init table, MADCTL/COLMOD, the address window and RAMWR.  The lower
 * owns the bus: CS/DC/backlight, SPI bytes, DMA kick/sync and a millisecond
 * delay.  A port provides three functions and nothing about the panel
 * sequence is duplicated in it.
 *
 * No state lives here: the lower decides when a panel is already up, so the
 * upper can be re-entered without re-initialising hardware. */

struct lcd_proto_ops {
    void (*cmd)(uint8_t c);
    void (*data)(const uint8_t *d, uint32_t n);
    void (*delay_ms)(uint32_t ms);
};

static inline void lcd_proto_cmd_data(const struct lcd_proto_ops *o,
                                      uint8_t cmd, const uint8_t *d,
                                      uint32_t n)
{
    o->cmd(cmd);
    if (n)
        o->data(d, n);
}

/* Address window in panel coordinates (offsets are the visible-area origin
 * for this 135x240 panel in landscape) and start RAMWR. */
static inline void lcd_proto_window(const struct lcd_proto_ops *o,
                                    int x, int y, int w, int h,
                                    int xoff, int yoff)
{
    uint8_t b[4];

    if (w <= 0 || h <= 0)
        return;
    b[0] = (uint8_t)((x + xoff) >> 8);
    b[1] = (uint8_t)(x + xoff);
    b[2] = (uint8_t)((x + xoff + w - 1) >> 8);
    b[3] = (uint8_t)(x + xoff + w - 1);
    lcd_proto_cmd_data(o, 0x2a, b, 4);       /* CASET */

    b[0] = (uint8_t)((y + yoff) >> 8);
    b[1] = (uint8_t)(y + yoff);
    b[2] = (uint8_t)((y + yoff + h - 1) >> 8);
    b[3] = (uint8_t)(y + yoff + h - 1);
    lcd_proto_cmd_data(o, 0x2b, b, 4);       /* RASET */

    o->cmd(0x2c);                            /* RAMWR */
}

/* Vendor init sequence, identical for the ATK-DNRP2350AM panel on both ISAs.
 * The caller brings up SPI/pins/backlight first and marks the panel up after
 * this returns. */
static inline void lcd_proto_panel_init(const struct lcd_proto_ops *o)
{
    static const uint8_t porch[]    = { 0x0c, 0x0c, 0x00, 0x33, 0x33 };
    static const uint8_t gctrl[]    = { 0x35 };
    static const uint8_t vcom[]     = { 0x19 };
    static const uint8_t lcmctl[]   = { 0x2c };
    static const uint8_t vdvvrhen[] = { 0x01 };
    static const uint8_t vrhs[]     = { 0x12 };
    static const uint8_t vdvs[]     = { 0x20 };
    static const uint8_t frctrl[]   = { 0x01 };
    static const uint8_t pwrctl[]   = { 0xa4, 0xa1 };
    static const uint8_t pvgam[]    = { 0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b,
                                        0x3f, 0x54, 0x4c, 0x18, 0x0d, 0x0b,
                                        0x1f, 0x23 };
    static const uint8_t nvgam[]    = { 0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c,
                                        0x3f, 0x44, 0x51, 0x2f, 0x1f, 0x1f,
                                        0x20, 0x23 };
    static const uint8_t colmod[]   = { 0x05 };   /* 16 bits per pixel */
    static const uint8_t madctl[]   = { 0x70 };   /* landscape, RGB order */

    o->delay_ms(20);

    o->cmd(0x01);                            /* SWRESET */
    o->delay_ms(150);
    o->cmd(0x11);                            /* SLPOUT  */
    o->delay_ms(120);

    lcd_proto_cmd_data(o, 0x36, madctl, sizeof(madctl));
    lcd_proto_cmd_data(o, 0x3a, colmod, sizeof(colmod));
    lcd_proto_cmd_data(o, 0xb2, porch, sizeof(porch));
    lcd_proto_cmd_data(o, 0xb7, gctrl, sizeof(gctrl));
    lcd_proto_cmd_data(o, 0xbb, vcom, sizeof(vcom));
    lcd_proto_cmd_data(o, 0xc0, lcmctl, sizeof(lcmctl));
    lcd_proto_cmd_data(o, 0xc2, vdvvrhen, sizeof(vdvvrhen));
    lcd_proto_cmd_data(o, 0xc3, vrhs, sizeof(vrhs));
    lcd_proto_cmd_data(o, 0xc4, vdvs, sizeof(vdvs));
    lcd_proto_cmd_data(o, 0xc6, frctrl, sizeof(frctrl));
    lcd_proto_cmd_data(o, 0xd0, pwrctl, sizeof(pwrctl));
    lcd_proto_cmd_data(o, 0xe0, pvgam, sizeof(pvgam));
    lcd_proto_cmd_data(o, 0xe1, nvgam, sizeof(nvgam));

    o->cmd(0x21);                            /* INVER = on: IPS panel */
    o->cmd(0x29);                            /* DISPON */
    o->delay_ms(20);
}

#endif
