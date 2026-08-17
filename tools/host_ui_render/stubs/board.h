#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#define BOARD_LCD_H_RES 172U
#define BOARD_LCD_V_RES 320U
#define BOARD_LCD_STRIP_LINES 20U
esp_err_t board_lcd_register_color_done_callback(esp_lcd_panel_io_color_trans_done_cb_t cb, void *ctx);
bool board_spi_acquire(TickType_t timeout);
void board_spi_release(void);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *data);
