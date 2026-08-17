#include "board_internal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define BOARD_PIN_SD_CS 4
#define BOARD_PIN_RGB 8
#define BOARD_PIN_BOOT_BUTTON 9
#define BOARD_PIN_LCD_CS 14
#define BOARD_PIN_LCD_DC 15
#define BOARD_PIN_LCD_RESET 21
#define BOARD_PIN_BACKLIGHT 22

#define BOARD_BACKLIGHT_MODE LEDC_LOW_SPEED_MODE
#define BOARD_BACKLIGHT_TIMER LEDC_TIMER_0
#define BOARD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BOARD_BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT
#define BOARD_BACKLIGHT_MAX_DUTY ((1U << 13U) - 1U)
#define BOARD_BACKLIGHT_FREQUENCY_HZ 5000U

static const char *TAG = "board_io";

esp_err_t board_internal_prepare_safe_pins(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << BOARD_PIN_SD_CS) |
                        (1ULL << BOARD_PIN_LCD_CS) |
                        (1ULL << BOARD_PIN_LCD_DC) |
                        (1ULL << BOARD_PIN_LCD_RESET) |
                        (1ULL << BOARD_PIN_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }

    /* Both devices must be deselected before SPI2 starts. */
    err = gpio_set_level(BOARD_PIN_LCD_CS, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(BOARD_PIN_SD_CS, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(BOARD_PIN_LCD_DC, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(BOARD_PIN_LCD_RESET, 0); /* active-low reset */
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(BOARD_PIN_BACKLIGHT, 0);
}

esp_err_t board_internal_backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BOARD_BACKLIGHT_MODE,
        .duty_resolution = BOARD_BACKLIGHT_RESOLUTION,
        .timer_num = BOARD_BACKLIGHT_TIMER,
        .freq_hz = BOARD_BACKLIGHT_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BOARD_PIN_BACKLIGHT,
        .speed_mode = BOARD_BACKLIGHT_MODE,
        .channel = BOARD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BOARD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err == ESP_OK) {
        g_board_state.backlight_ready = true;
        g_board_state.brightness_percent = 0;
    }
    return err;
}

esp_err_t board_internal_backlight_set(uint8_t percent)
{
    if (!g_board_state.backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t applied = percent > BOARD_BACKLIGHT_MAX_PERCENT
                                ? BOARD_BACKLIGHT_MAX_PERCENT
                                : percent;
    const uint32_t duty =
        ((uint32_t)applied * BOARD_BACKLIGHT_MAX_DUTY + 50U) / 100U;
    esp_err_t err = ledc_set_duty(BOARD_BACKLIGHT_MODE,
                                  BOARD_BACKLIGHT_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_update_duty(BOARD_BACKLIGHT_MODE, BOARD_BACKLIGHT_CHANNEL);
    if (err == ESP_OK) {
        g_board_state.brightness_percent = applied;
        if (applied != percent) {
            ESP_LOGW(TAG, "Backlight request %u%% clamped to %u%%", percent, applied);
        }
    }
    return err;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    if (!g_board_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return board_internal_backlight_set(percent);
}

uint8_t board_backlight_get(void)
{
    return g_board_state.brightness_percent;
}

void board_internal_backlight_deinit(void)
{
    if (!g_board_state.backlight_ready) {
        return;
    }
    ledc_stop(BOARD_BACKLIGHT_MODE, BOARD_BACKLIGHT_CHANNEL, 0);
    gpio_set_level(BOARD_PIN_BACKLIGHT, 0);
    g_board_state.backlight_ready = false;
    g_board_state.brightness_percent = 0;
}

esp_err_t board_internal_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        g_board_state.button_ready = true;
    }
    return err;
}

bool board_boot_button_is_pressed(void)
{
    return g_board_state.button_ready && gpio_get_level(BOARD_PIN_BOOT_BUTTON) == 0;
}

void board_internal_button_deinit(void)
{
    if (g_board_state.button_ready) {
        gpio_reset_pin(BOARD_PIN_BOOT_BUTTON);
        g_board_state.button_ready = false;
    }
}

esp_err_t board_internal_rgb_init(void)
{
    if (g_board_state.rgb_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_PIN_RGB,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10U * 1000U * 1000U,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &g_board_state.rgb_strip);
    if (err != ESP_OK) {
        g_board_state.rgb_strip = NULL;
        return err;
    }
    err = led_strip_clear(g_board_state.rgb_strip);
    if (err != ESP_OK) {
        led_strip_del(g_board_state.rgb_strip);
        g_board_state.rgb_strip = NULL;
        return err;
    }
    g_board_state.rgb_ready = true;
    return ESP_OK;
}

esp_err_t board_rgb_init(void)
{
    if (!g_board_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    g_board_state.rgb_init_attempted = true;
    g_board_state.rgb_init_result = board_internal_rgb_init();
    return g_board_state.rgb_init_result;
}

esp_err_t board_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!g_board_state.rgb_ready || g_board_state.rgb_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = led_strip_set_pixel(g_board_state.rgb_strip, 0, red, green, blue);
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_refresh(g_board_state.rgb_strip);
}

esp_err_t board_rgb_clear(void)
{
    if (!g_board_state.rgb_ready || g_board_state.rgb_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(g_board_state.rgb_strip);
}

void board_internal_rgb_deinit(void)
{
    if (g_board_state.rgb_strip != NULL) {
        led_strip_clear(g_board_state.rgb_strip);
        led_strip_del(g_board_state.rgb_strip);
        g_board_state.rgb_strip = NULL;
    }
    g_board_state.rgb_ready = false;
}
