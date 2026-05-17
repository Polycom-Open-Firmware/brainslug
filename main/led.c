/* WS2812 onboard RGB LED control (Waveshare ESP32-S3-ETH/PoE-Cam: GPIO21).
 *
 * The board's WS2812 latches its colour from a serial frame and powers
 * up full-white with no controller driving DIN — neither GPIO-input nor
 * a power-cycle turns it off; only clocking it a {0,0,0} frame does.
 * We blank it at boot and expose /led for runtime control.
 */
#include "led.h"
#include "led_strip.h"
#include "esp_log.h"

#define WS2812_GPIO   21
#define WS2812_COUNT  1

static const char *TAG = "led";
static led_strip_handle_t s_strip;

void led_init(void)
{
    led_strip_config_t scfg = {
        .strip_gpio_num   = WS2812_GPIO,
        .max_leds         = WS2812_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags            = { .invert_out = false },
    };
    led_strip_rmt_config_t rcfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags         = { .with_dma = false },
    };
    if (led_strip_new_rmt_device(&scfg, &rcfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "ws2812 init failed (gpio %d)", WS2812_GPIO);
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);          /* off */
    ESP_LOGI(TAG, "ws2812 gpio %d -> off", WS2812_GPIO);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return;
    if (!r && !g && !b) {
        led_strip_clear(s_strip);
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
