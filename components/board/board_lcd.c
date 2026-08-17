/*
 * The initialization sequence in this file is derived from Waveshare's
 * ESP32-C6-LCD-1.47 native ESP-IDF demo, whose ST7789T driver carries:
 *
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * AirTrack retains the native sequence and explicitly exposes its unusual D0
 * parameter count for hardware validation. Error handling and the surrounding
 * esp_lcd integration are new for this board component.
 */

#include "board_internal.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_PIN_LCD_CS 14
#define BOARD_PIN_LCD_DC 15
#define BOARD_PIN_LCD_RESET 21
#define BOARD_LCD_PIXEL_CLOCK_HZ (12U * 1000U * 1000U)
#define BOARD_LCD_QUEUE_DEPTH 3U

#define ST7789_CMD_RAMCTRL 0xB0

static const char *TAG = "board_lcd";

typedef struct {
    uint8_t command;
    uint8_t data[14];
    uint8_t data_len;
    uint16_t delay_ms;
} st7789_init_command_t;

static const st7789_init_command_t s_waveshare_init[] = {
    {LCD_CMD_SLPOUT, {0}, 0, 100},
    {LCD_CMD_MADCTL, {0x00}, 1, 0},
    {LCD_CMD_COLMOD, {0x55}, 1, 0},
    {ST7789_CMD_RAMCTRL, {0x00, 0xE8}, 2, 0},
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, {0x75}, 1, 0},
    {0xBB, {0x1A}, 1, 0},
    {0xC0, {0x80}, 1, 0},
    {0xC2, {0x01, 0xFF}, 2, 0},
    {0xC3, {0x13}, 1, 0},
    {0xC4, {0x20}, 1, 0},
    {0xC6, {0x0F}, 1, 0},
    {0xD0, {0xA4, 0xA1}, 2, 0},
    {0xE0, {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
            0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 14, 0},
    {0xE1, {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
            0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E}, 14, 0},
    {LCD_CMD_INVON, {0}, 0, 0},
    {LCD_CMD_DISPON, {0}, 0, 0},
    {LCD_CMD_RAMWR, {0}, 0, 0},
};

static esp_err_t send_waveshare_init(uint8_t d0_param_count)
{
    for (size_t i = 0; i < sizeof(s_waveshare_init) / sizeof(s_waveshare_init[0]); ++i) {
        const st7789_init_command_t *entry = &s_waveshare_init[i];
        size_t data_len = entry->data_len;
        if (entry->command == 0xD0) {
            data_len = d0_param_count;
        }

        const esp_err_t err = esp_lcd_panel_io_tx_param(
            g_board_state.panel_io, entry->command,
            data_len > 0 ? entry->data : NULL, data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ST7789 command 0x%02X failed: %s",
                     entry->command, esp_err_to_name(err));
            return err;
        }
        if (entry->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t clear_first_frame(void)
{
    uint16_t *strip = heap_caps_calloc(
        BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES, sizeof(*strip),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (strip == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (uint32_t y = 0; y < BOARD_LCD_V_RES; y += BOARD_LCD_STRIP_LINES) {
        uint32_t y_end = y + BOARD_LCD_STRIP_LINES;
        if (y_end > BOARD_LCD_V_RES) {
            y_end = BOARD_LCD_V_RES;
        }
        err = esp_lcd_panel_draw_bitmap(g_board_state.panel, 0, (int)y,
                                        BOARD_LCD_H_RES, (int)y_end, strip);
        if (err != ESP_OK) {
            break;
        }
    }

    /* Parameter transfers wait for all queued color transfers, then send NOP. */
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_tx_param(g_board_state.panel_io, LCD_CMD_NOP, NULL, 0);
    }
    heap_caps_free(strip);
    return err;
}

esp_err_t board_internal_lcd_init(uint8_t d0_param_count)
{
    if (!g_board_state.spi_ready || g_board_state.panel_io != NULL ||
        g_board_state.panel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d0_param_count != BOARD_ST7789_D0_NATIVE_PARAM_COUNT &&
        d0_param_count != BOARD_ST7789_D0_ARDUINO_PARAM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(g_board_state.spi_gate, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = BOARD_LCD_QUEUE_DEPTH,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST,
                                   &io_config, &g_board_state.panel_io);
    if (err != ESP_OK) {
        goto done;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_PIN_LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .flags.reset_active_high = false,
    };
    err = esp_lcd_new_panel_st7789(g_board_state.panel_io, &panel_config,
                                   &g_board_state.panel);
    if (err != ESP_OK) {
        goto done;
    }

    err = esp_lcd_panel_reset(g_board_state.panel);
    if (err != ESP_OK) {
        goto done;
    }
    err = send_waveshare_init(d0_param_count);
    if (err != ESP_OK) {
        goto done;
    }
    err = esp_lcd_panel_set_gap(g_board_state.panel, BOARD_LCD_X_GAP,
                                BOARD_LCD_Y_GAP);
    if (err != ESP_OK) {
        goto done;
    }
    err = esp_lcd_panel_mirror(g_board_state.panel, true, false);
    if (err != ESP_OK) {
        goto done;
    }
    err = clear_first_frame();
    if (err != ESP_OK) {
        goto done;
    }

    g_board_state.lcd_ready = true;
    ESP_LOGI(TAG, "ST7789 ready at 12 MHz, BGR/mirror-X, gap=(%u,%u)",
             BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP);

done:
    if (err != ESP_OK) {
        if (g_board_state.panel != NULL) {
            esp_lcd_panel_del(g_board_state.panel);
            g_board_state.panel = NULL;
        }
        if (g_board_state.panel_io != NULL) {
            esp_lcd_panel_io_del(g_board_state.panel_io);
            g_board_state.panel_io = NULL;
        }
    }
    xSemaphoreGive(g_board_state.spi_gate);
    return err;
}

void board_internal_lcd_deinit(void)
{
    if (g_board_state.panel == NULL && g_board_state.panel_io == NULL) {
        return;
    }

    if (g_board_state.spi_gate != NULL &&
        xSemaphoreTake(g_board_state.spi_gate, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (g_board_state.panel != NULL) {
        esp_lcd_panel_disp_on_off(g_board_state.panel, false);
        esp_lcd_panel_del(g_board_state.panel);
        g_board_state.panel = NULL;
    }
    if (g_board_state.panel_io != NULL) {
        esp_lcd_panel_io_del(g_board_state.panel_io);
        g_board_state.panel_io = NULL;
    }
    g_board_state.lcd_ready = false;
    if (g_board_state.spi_gate != NULL) {
        xSemaphoreGive(g_board_state.spi_gate);
    }
}
