/* Hardware natives for pvm: GPIO, ADC and PWM.  These are always available;
 * they are tiny and do not depend on the float runtime. */
#include "amxcfg.h"
#include "lib.h"
#include "amx.h"

static cell n_gpio_count(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return gpio_count();
}

static cell n_gpio_mode(AMX *amx, const cell *p)
{
    (void)amx;
    return gpio_mode((int)p[1], (int)p[2]);
}

static cell n_gpio_write(AMX *amx, const cell *p)
{
    (void)amx;
    return gpio_write((int)p[1], (int)p[2]);
}

static cell n_gpio_read(AMX *amx, const cell *p)
{
    (void)amx;
    return gpio_read((int)p[1]);
}

static cell n_adc_count(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return adc_count();
}

static cell n_adc_read(AMX *amx, const cell *p)
{
    (void)amx;
    return adc_read((int)p[1]);
}

static cell n_adc_vref_mv(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return adc_vref_mv();
}

static cell n_adc_bits(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return adc_bits();
}

static cell n_pwm_count(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return pwm_count();
}

static cell n_pwm_set(AMX *amx, const cell *p)
{
    (void)amx;
    return pwm_set((int)p[1], (int)p[2], (int)p[3]);
}

static cell n_pwm_off(AMX *amx, const cell *p)
{
    (void)amx;
    return pwm_off((int)p[1]);
}

const AMX_NATIVE_INFO pvm_hw_natives[] = {
    { "gpio_count", n_gpio_count },
    { "gpio_mode", n_gpio_mode },
    { "gpio_write", n_gpio_write },
    { "gpio_read", n_gpio_read },
    { "adc_count", n_adc_count },
    { "adc_read", n_adc_read },
    { "adc_vref_mv", n_adc_vref_mv },
    { "adc_bits", n_adc_bits },
    { "pwm_count", n_pwm_count },
    { "pwm_set", n_pwm_set },
    { "pwm_off", n_pwm_off },
    { NULL, NULL }
};
