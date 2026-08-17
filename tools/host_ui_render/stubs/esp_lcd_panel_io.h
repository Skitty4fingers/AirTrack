#pragma once
#include <stdbool.h>
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct { int dummy; } esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
