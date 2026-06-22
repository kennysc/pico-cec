#include "hardware/clocks.h"
#include "ws2812.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

static PIO ws2812_pio = pio0;
static uint ws2812_sm;

static inline void ws2812_program_init(PIO pio, uint sm, uint offset, uint pin, float freq, bool rgbw) {
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, false, true, rgbw ? 32 : 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
    float div = (float)clock_get_hz(clk_sys) / (freq * cycles_per_bit);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

void ws2812_init(unsigned int gpio_pin) {
    uint offset = pio_add_program(ws2812_pio, &ws2812_program);
    ws2812_sm = pio_claim_unused_sm(ws2812_pio, true);
    ws2812_program_init(ws2812_pio, ws2812_sm, offset, gpio_pin, 800000, false);
    ws2812_set_rgb(0, 0, 0);
}

void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    pio_sm_put_blocking(ws2812_pio, ws2812_sm, grb << 8u);
}
