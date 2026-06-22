#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>
#include <stdbool.h>

void ws2812_init(unsigned int gpio_pin);
void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Pre-defined colours for status indication */
#define WS2812_BLACK   0x000000
#define WS2812_RED     0xFF0000
#define WS2812_GREEN   0x00FF00
#define WS2812_BLUE    0x0000FF
#define WS2812_YELLOW  0xFFFF00
#define WS2812_CYAN    0x00FFFF
#define WS2812_WHITE   0xFFFFFF

static inline void ws2812_set_hex(uint32_t rgb) {
    ws2812_set_rgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

#endif
