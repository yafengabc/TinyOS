/* Minimal RP2040 USB CDC-ACM console (see usb_cdc.c). */

void usb_cdc_console_init(void);
void usb_cdc_putc(char c);
void usb_cdc_flush(void);
int  usb_cdc_getchar(void);
int  usb_cdc_configured(void);   /* 1 once the host has SET_CONFIGURATION */
