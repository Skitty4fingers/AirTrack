/*
 * CH32V003 I/O expander on the Waveshare ESP32-C6-Touch-LCD-2.8.
 *
 * The register map and the reset timings below follow Waveshare's Arduino
 * board-support sources for this board (io_extension.cpp / Board_IO.cpp).
 * Three signals AirTrack cares about are only reachable through it:
 *
 *   EXIO1  LCD reset       driven low, then high, before the panel init table
 *   PWM    LCD backlight   an 8-bit duty register, not an ESP32 LEDC channel
 *   EXIO0  touch reset     held released; AirTrack does not use the panel
 *
 * Because the backlight is an I2C write rather than a PWM peripheral, every
 * brightness change costs a short bus transaction.  The supervisor only
 * writes brightness when the computed value actually changes, so the night
 * schedule and the dashboard slider stay cheap.
 *
 * The bus and the expander are inseparable on this board - the panel cannot be
 * brought up without both - so one guard covers the whole file and the build
 * only compiles it for boards that have them.
 */

#include "board_internal.h"

#if BOARD_HAS_I2C_BUS && BOARD_HAS_IO_EXPANDER

#include "board_profile.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EXIO_I2C_ADDRESS 0x24
#define EXIO_I2C_TIMEOUT_MS 100

/* Register map. */
#define EXIO_REG_MODE 0x02
#define EXIO_REG_OUTPUT 0x03
#define EXIO_REG_INPUT 0x04
#define EXIO_REG_PWM 0x05
#define EXIO_REG_ADC 0x06

/* Every expander channel is an output for AirTrack's purposes. */
#define EXIO_MODE_ALL_OUTPUTS 0xFF

/* Vendor reset timings, in milliseconds. */
#define EXIO_LCD_RESET_LOW_MS 50U
#define EXIO_LCD_RESET_SETTLE_MS 120U

static const char *TAG = "board_exio";

static esp_err_t exio_read(uint8_t reg, uint8_t *data, size_t length)
{
    if (g_board_state.exio_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(g_board_state.exio_device, &reg, 1,
                                       data, length, EXIO_I2C_TIMEOUT_MS);
}

static esp_err_t exio_write(uint8_t reg, uint8_t value)
{
    if (g_board_state.exio_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(g_board_state.exio_device, payload,
                               sizeof(payload), EXIO_I2C_TIMEOUT_MS);
}

esp_err_t board_internal_i2c_init(void)
{
    if (g_board_state.i2c_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &g_board_state.i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared I2C bus unavailable: %s", esp_err_to_name(err));
        g_board_state.i2c_bus = NULL;
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXIO_I2C_ADDRESS,
        .scl_speed_hz = BOARD_I2C_FREQUENCY_HZ,
    };
    err = i2c_master_bus_add_device(g_board_state.i2c_bus, &device_config,
                                    &g_board_state.exio_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I/O expander not attachable: %s", esp_err_to_name(err));
        g_board_state.exio_device = NULL;
        i2c_del_master_bus(g_board_state.i2c_bus);
        g_board_state.i2c_bus = NULL;
        return err;
    }

    /*
     * Drive every channel as an output and publish a known state: both resets
     * released (high) and the audio amplifier enabled, matching the vendor
     * power-on value.  The panel is reset explicitly later, from the LCD path.
     */
    g_board_state.exio_outputs = (uint8_t)((1U << BOARD_EXIO_TOUCH_RESET) |
                                           (1U << BOARD_EXIO_LCD_RESET) |
                                           (1U << BOARD_EXIO_AUDIO_ENABLE));
    err = exio_write(EXIO_REG_MODE, EXIO_MODE_ALL_OUTPUTS);
    if (err == ESP_OK) {
        err = exio_write(EXIO_REG_OUTPUT, g_board_state.exio_outputs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I/O expander at 0x%02X did not answer: %s",
                 EXIO_I2C_ADDRESS, esp_err_to_name(err));
        i2c_master_bus_rm_device(g_board_state.exio_device);
        g_board_state.exio_device = NULL;
        i2c_del_master_bus(g_board_state.i2c_bus);
        g_board_state.i2c_bus = NULL;
        return err;
    }

    /* The backlight must stay dark until the first frame has been drawn. */
    err = exio_write(EXIO_REG_PWM, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Backlight could not be blanked at start-up: %s",
                 esp_err_to_name(err));
    }

    g_board_state.exio_ready = true;
    ESP_LOGI(TAG, "I/O expander ready at 0x%02X on SDA=%d SCL=%d (%lu kHz)",
             EXIO_I2C_ADDRESS, BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL,
             (unsigned long)(BOARD_I2C_FREQUENCY_HZ / 1000U));
    return ESP_OK;
}

void board_internal_i2c_deinit(void)
{
    if (g_board_state.exio_device != NULL) {
        i2c_master_bus_rm_device(g_board_state.exio_device);
        g_board_state.exio_device = NULL;
    }
    if (g_board_state.i2c_bus != NULL) {
        i2c_del_master_bus(g_board_state.i2c_bus);
        g_board_state.i2c_bus = NULL;
    }
    g_board_state.exio_ready = false;
}

esp_err_t board_internal_exio_set(uint8_t channel, bool level)
{
    if (!g_board_state.exio_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel > 7U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t updated = level
        ? (uint8_t)(g_board_state.exio_outputs | (1U << channel))
        : (uint8_t)(g_board_state.exio_outputs & ~(1U << channel));
    const esp_err_t err = exio_write(EXIO_REG_OUTPUT, updated);
    if (err == ESP_OK) {
        g_board_state.exio_outputs = updated;
    }
    return err;
}

esp_err_t board_internal_exio_set_pwm(uint8_t duty)
{
    return exio_write(EXIO_REG_PWM, duty);
}

esp_err_t board_internal_exio_reset_panel(void)
{
    esp_err_t err = board_internal_exio_set(BOARD_EXIO_LCD_RESET, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(EXIO_LCD_RESET_LOW_MS));
    err = board_internal_exio_set(BOARD_EXIO_LCD_RESET, true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(EXIO_LCD_RESET_SETTLE_MS));
    return ESP_OK;
}

#if BOARD_HAS_BATTERY_SENSE

/*
 * Scaling for the battery sense rail.
 *
 * The expander is a CH32V003, whose ADC is 10-bit against its 3.3 V supply,
 * and the battery reaches it through the usual 3:1 divider Waveshare use on
 * these boards.  Neither figure is published for this board; both were
 * derived on hardware and cross-checked against a measured cell, so
 * board_battery_t keeps the raw counts for re-verification.
 */
#define BATTERY_ADC_FULL_SCALE 1023.0f
#define BATTERY_ADC_REFERENCE_V 3.3f
#define BATTERY_DIVIDER_RATIO 3.0f
/* Below this the rail is not a live cell: no battery, or one that is flat. */
#define BATTERY_MINIMUM_PLAUSIBLE_V 2.6f

/*
 * Open-circuit discharge curve for one lithium-ion cell.  A linear map from
 * volts to percent is badly wrong in the middle of the range, where the curve
 * is nearly flat, so this interpolates between measured points instead.
 */
static uint8_t battery_percent_from_volts(float volts)
{
    static const struct {
        float volts;
        uint8_t percent;
    } curve[] = {
        {4.20f, 100U}, {4.10f, 92U}, {4.00f, 81U}, {3.90f, 68U},
        {3.85f, 60U},  {3.80f, 50U}, {3.75f, 41U}, {3.70f, 32U},
        {3.65f, 23U},  {3.60f, 15U}, {3.50f, 7U},  {3.40f, 3U},
        {3.30f, 0U},
    };
    const size_t count = sizeof(curve) / sizeof(curve[0]);
    if (volts >= curve[0].volts) {
        return 100U;
    }
    for (size_t i = 1U; i < count; ++i) {
        if (volts >= curve[i].volts) {
            const float span = curve[i - 1U].volts - curve[i].volts;
            const float above = volts - curve[i].volts;
            const float range =
                (float)(curve[i - 1U].percent - curve[i].percent);
            return (uint8_t)(curve[i].percent + (above / span) * range + 0.5f);
        }
    }
    return 0U;
}

esp_err_t board_battery_read(board_battery_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (board_battery_t){0};
    out->usb_present = usb_serial_jtag_is_connected();

    uint16_t counts = 0U;
    const esp_err_t err = board_battery_raw(&counts, NULL);
    if (err != ESP_OK) {
        return err;
    }
    out->adc_counts = counts;
    out->volts = ((float)counts / BATTERY_ADC_FULL_SCALE) *
                 BATTERY_ADC_REFERENCE_V * BATTERY_DIVIDER_RATIO;
    if (out->volts >= BATTERY_MINIMUM_PLAUSIBLE_V) {
        out->valid = true;
        out->percent = battery_percent_from_volts(out->volts);
    }
    return ESP_OK;
}

esp_err_t board_battery_raw(uint16_t *adc_counts, uint8_t *expander_inputs)
{
    if (!g_board_state.exio_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (adc_counts != NULL) {
        uint8_t raw[2] = {0, 0};
        const esp_err_t err = exio_read(EXIO_REG_ADC, raw, sizeof(raw));
        if (err != ESP_OK) {
            return err;
        }
        /* Little-endian, matching the vendor helper. */
        *adc_counts = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    }
    if (expander_inputs != NULL) {
        const esp_err_t err = exio_read(EXIO_REG_INPUT, expander_inputs, 1);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
#endif

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return g_board_state.i2c_bus;
}

#endif /* BOARD_HAS_I2C_BUS && BOARD_HAS_IO_EXPANDER */
