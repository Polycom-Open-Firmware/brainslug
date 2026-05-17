/* WS2812 onboard RGB LED control. */
#pragma once
#include <stdint.h>

/* Init the WS2812 (GPIO21) and blank it. Safe no-op if RMT alloc fails. */
void led_init(void);

/* Set the LED. (0,0,0) = off. */
void led_set(uint8_t r, uint8_t g, uint8_t b);
