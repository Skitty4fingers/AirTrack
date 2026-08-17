#include "board.h"

#include <string.h>

#include "board_internal.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/task.h"

#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_PIN_SPI_MOSI 6
#define BOARD_PIN_SPI_MISO 5
#define BOARD_PIN_SPI_SCLK 7

static const char *TAG = "board";

board_state_t g_board_state;

_Static_assert(BOARD_LCD_STRIP_BYTES <= BOARD_SPI_MAX_TRANSFER_BYTES,
               "LCD strip must fit in one SPI transfer");

static void board_cleanup(void)
{
    board_internal_backlight_set(0);
    board_internal_rgb_deinit();
    board_internal_lcd_deinit();
    board_internal_sd_unmount();

    if (g_board_state.spi_ready) {
        const esp_err_t err = spi_bus_free(BOARD_SPI_HOST);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPI2 cleanup failed: %s", esp_err_to_name(err));
        }
        g_board_state.spi_ready = false;
    }

    board_internal_button_deinit();
    board_internal_backlight_deinit();

    if (g_board_state.spi_gate != NULL) {
        vSemaphoreDelete(g_board_state.spi_gate);
        g_board_state.spi_gate = NULL;
    }
}

esp_err_t board_init(const board_config_t *config)
{
    if (g_board_state.initialized || g_board_state.init_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    const board_config_t defaults = BOARD_CONFIG_DEFAULT();
    const board_config_t effective = config != NULL ? *config : defaults;
    if (effective.st7789_d0_param_count != BOARD_ST7789_D0_NATIVE_PARAM_COUNT &&
        effective.st7789_d0_param_count != BOARD_ST7789_D0_ARDUINO_PARAM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&g_board_state, 0, sizeof(g_board_state));
    g_board_state.init_in_progress = true;
    g_board_state.sd_mount_result = ESP_ERR_NOT_FINISHED;
    g_board_state.rgb_init_result = ESP_ERR_NOT_FINISHED;
    g_board_state.st7789_d0_param_count = effective.st7789_d0_param_count;

    g_board_state.spi_gate = xSemaphoreCreateMutexStatic(&g_board_state.spi_gate_storage);
    if (g_board_state.spi_gate == NULL) {
        memset(&g_board_state, 0, sizeof(g_board_state));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = board_internal_prepare_safe_pins();
    if (err != ESP_OK) {
        goto fail;
    }
    err = board_internal_backlight_init();
    if (err != ESP_OK) {
        goto fail;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_PIN_SPI_MOSI,
        .miso_io_num = BOARD_PIN_SPI_MISO,
        .sclk_io_num = BOARD_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER_BYTES,
    };
    err = spi_bus_initialize(BOARD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 initialization failed: %s", esp_err_to_name(err));
        goto fail;
    }
    g_board_state.spi_ready = true;

    if (effective.mount_sd) {
        g_board_state.sd_mount_attempted = true;
        g_board_state.sd_mount_result = board_internal_sd_mount();
        if (g_board_state.sd_mount_result != ESP_OK) {
            ESP_LOGW(TAG, "Optional SD unavailable: %s",
                     esp_err_to_name(g_board_state.sd_mount_result));
        }
    }

    err = board_internal_lcd_init(effective.st7789_d0_param_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD initialization failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = board_internal_button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button initialization failed: %s", esp_err_to_name(err));
        goto fail;
    }

    if (effective.init_rgb) {
        g_board_state.rgb_init_attempted = true;
        g_board_state.rgb_init_result = board_internal_rgb_init();
        if (g_board_state.rgb_init_result != ESP_OK) {
            ESP_LOGW(TAG, "Optional RGB LED unavailable: %s",
                     esp_err_to_name(g_board_state.rgb_init_result));
        }
    }

    err = board_internal_backlight_set(effective.startup_brightness_percent);
    if (err != ESP_OK) {
        goto fail;
    }

    g_board_state.initialized = true;
    g_board_state.init_in_progress = false;
    ESP_LOGI(TAG, "Board ready (LCD %ux%u, D0 parameters=%u, SD=%s, brightness=%u%%)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, g_board_state.st7789_d0_param_count,
             g_board_state.sd_mounted ? "mounted" : "unavailable",
             g_board_state.brightness_percent);
    return ESP_OK;

fail:
    board_cleanup();
    memset(&g_board_state, 0, sizeof(g_board_state));
    return err;
}

esp_err_t board_deinit(void)
{
    if (!g_board_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    board_cleanup();
    memset(&g_board_state, 0, sizeof(g_board_state));
    return ESP_OK;
}

bool board_is_initialized(void)
{
    return g_board_state.initialized;
}

esp_err_t board_get_status(board_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *status = (board_status_t) {
        .initialized = g_board_state.initialized,
        .spi_ready = g_board_state.spi_ready,
        .lcd_ready = g_board_state.lcd_ready,
        .sd_mount_attempted = g_board_state.sd_mount_attempted,
        .sd_mounted = g_board_state.sd_mounted,
        .rgb_init_attempted = g_board_state.rgb_init_attempted,
        .rgb_ready = g_board_state.rgb_ready,
        .button_ready = g_board_state.button_ready,
        .sd_mount_result = g_board_state.sd_mount_result,
        .rgb_init_result = g_board_state.rgb_init_result,
        .sd_capacity_bytes = g_board_state.sd_capacity_bytes,
        .brightness_percent = g_board_state.brightness_percent,
        .st7789_d0_param_count = g_board_state.st7789_d0_param_count,
    };
    return ESP_OK;
}

esp_lcd_panel_handle_t board_lcd_panel(void)
{
    return g_board_state.lcd_ready ? g_board_state.panel : NULL;
}

esp_lcd_panel_io_handle_t board_lcd_panel_io(void)
{
    return g_board_state.lcd_ready ? g_board_state.panel_io : NULL;
}

esp_err_t board_lcd_register_color_done_callback(
    esp_lcd_panel_io_color_trans_done_cb_t callback,
    void *user_ctx)
{
    if (!g_board_state.lcd_ready || g_board_state.panel_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = callback,
    };
    return esp_lcd_panel_io_register_event_callbacks(g_board_state.panel_io,
                                                      &callbacks, user_ctx);
}

bool board_spi_acquire(TickType_t timeout_ticks)
{
    if (!g_board_state.spi_ready || g_board_state.spi_gate == NULL) {
        return false;
    }
    return xSemaphoreTake(g_board_state.spi_gate, timeout_ticks) == pdTRUE;
}

void board_spi_release(void)
{
    if (g_board_state.spi_gate == NULL) {
        return;
    }
    if (xSemaphoreGetMutexHolder(g_board_state.spi_gate) != xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "SPI gate release attempted by a non-owner task");
        return;
    }
    xSemaphoreGive(g_board_state.spi_gate);
}

bool board_sd_is_mounted(void)
{
    return g_board_state.sd_mounted;
}

const sdmmc_card_t *board_sd_card(void)
{
    return g_board_state.sd_mounted ? g_board_state.sd_card : NULL;
}

const char *board_sd_mount_point(void)
{
    return BOARD_SD_MOUNT_POINT;
}
