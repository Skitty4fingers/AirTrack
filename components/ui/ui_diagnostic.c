#include "ui_diagnostic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "qrcode.h"
#include "ui_icons.h"

#define UI_FLUSH_NOTIFY_INDEX 1U
#define UI_FLUSH_TIMEOUT_MS 1000U
#define UI_QR_CANVAS_SIZE 140U
#define UI_QR_QUIET_ZONE_MODULES 4U
#define UI_QR_MAX_VERSION 11
#define UI_WIFI_SSID_MAX_BYTES 32U
#define UI_WIFI_PASSWORD_MIN_BYTES 8U
#define UI_WIFI_PASSWORD_MAX_BYTES 63U
#define UI_WIFI_QR_PAYLOAD_BYTES 224U
#define UI_RADAR_SWEEP_MS 6000U
#define UI_STALE_AGE_S 30.0

#define UI_COLOR_BG 0x07111F
#define UI_COLOR_PANEL 0x0D1A2B
#define UI_COLOR_TEXT 0xF2F6FC
#define UI_COLOR_DIM 0xA7B5CA
#define UI_COLOR_MUTED 0x6F819B
#define UI_COLOR_CYAN 0x55D9F3
#define UI_COLOR_GREEN 0x8BE36D
#define UI_COLOR_AMBER 0xFFB454
#define UI_COLOR_RED 0xFF647C

#if CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES <= UI_FLUSH_NOTIFY_INDEX
#error "The UI flush notification requires FreeRTOS task notification index 1"
#endif

static const char *TAG = "ui_diag";

typedef struct {
    bool initialized;
    bool port_started;
    bool callback_registered;
    bool diagnostic_visible;
    bool tracking_visible;
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    void *draw_buffer_a;
    void *draw_buffer_b;
    lv_obj_t *screen;
    lv_obj_t *phase;
    lv_obj_t *lcd_value;
    lv_obj_t *sd_value;
    lv_obj_t *flash_value;
    lv_obj_t *ssid;
    lv_obj_t *ip;
    /* Tracking screen widgets. */
    lv_obj_t *hdr_wifi;
    lv_obj_t *hdr_dot;
    lv_obj_t *hdr_right;
    lv_obj_t *trk_data;          /* container shown while a target exists */
    lv_obj_t *trk_identity;
    lv_obj_t *trk_meta;
    lv_obj_t *trk_divider;
    lv_obj_t *trk_distance;
    lv_obj_t *trk_unit;
    lv_obj_t *trk_scale;
    lv_obj_t *trk_arc;
    lv_obj_t *trk_plane;
    lv_obj_t *trk_arrow;
    lv_obj_t *trk_row_icon[4];
    lv_obj_t *trk_row_text[4];
    lv_obj_t *trk_from_caption;
    lv_obj_t *trk_from;
    lv_obj_t *trk_to_caption;
    lv_obj_t *trk_to;
    lv_obj_t *trk_empty;         /* container shown without a target */
    lv_obj_t *trk_empty_head;
    lv_obj_t *trk_radar_ring[4];
    lv_obj_t *trk_radar_sweep;
    lv_obj_t *trk_radar_dot;
    lv_obj_t *trk_empty_within;
    lv_obj_t *trk_empty_radius;
    lv_obj_t *trk_empty_unit;
    lv_obj_t *trk_empty_hint;
    lv_obj_t *trk_footer_net;
    lv_obj_t *trk_footer_data;
    bool radar_animating;
    lv_point_precise_t arrow_points[5];
    lv_draw_buf_t *qr_draw_buffer;
} ui_context_t;

typedef struct {
    lv_obj_t *canvas;
    bool rendered;
} qr_render_context_t;

static ui_context_t s_ui;
static volatile TaskHandle_t s_flush_waiter;

static void radar_sweep_animate(void *object, int32_t value);

static bool validate_printable_bytes(const char *value, size_t min_length,
                                     size_t max_length)
{
    if (value == NULL) {
        return false;
    }

    const size_t length = strnlen(value, max_length + 1U);
    if (length < min_length || length > max_length) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t byte = (uint8_t)value[index];
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool validate_ipv4_address(const char *address)
{
    if (address == NULL) {
        return false;
    }

    const char *cursor = address;
    for (unsigned int part = 0; part < 4U; ++part) {
        unsigned int value = 0;
        unsigned int digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = (value * 10U) + (unsigned int)(*cursor - '0');
            ++digits;
            ++cursor;
            if (digits > 3U || value > 255U) {
                return false;
            }
        }
        if (digits == 0U) {
            return false;
        }
        if (part < 3U) {
            if (*cursor != '.') {
                return false;
            }
            ++cursor;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

static bool wifi_qr_escape(const char *input, char *output,
                           size_t output_size)
{
    size_t output_index = 0;
    for (size_t input_index = 0; input[input_index] != '\0'; ++input_index) {
        const char byte = input[input_index];
        const bool reserved = byte == '\\' || byte == ';' || byte == ',' ||
                              byte == ':' || byte == '"';
        const size_t bytes_needed = reserved ? 2U : 1U;
        if (output_index + bytes_needed >= output_size) {
            return false;
        }
        if (reserved) {
            output[output_index++] = '\\';
        }
        output[output_index++] = byte;
    }
    output[output_index] = '\0';
    return true;
}

static void clear_sensitive_buffer(void *buffer, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    while (length-- > 0U) {
        *bytes++ = 0;
    }
}

static void render_qr_to_canvas(esp_qrcode_handle_t qrcode, void *user_data)
{
    qr_render_context_t *context = (qr_render_context_t *)user_data;
    if (qrcode == NULL || context == NULL || context->canvas == NULL) {
        return;
    }

    const int qr_modules = esp_qrcode_get_size(qrcode);
    if (qr_modules <= 0) {
        return;
    }
    const int total_modules =
        qr_modules + (2 * (int)UI_QR_QUIET_ZONE_MODULES);
    const int scale = (int)UI_QR_CANVAS_SIZE / total_modules;
    if (scale < 1) {
        return;
    }

    lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(context->canvas);
    if (draw_buffer == NULL ||
        draw_buffer->header.cf != LV_COLOR_FORMAT_I1) {
        return;
    }

    lv_canvas_set_palette(context->canvas, 0,
                          lv_color_to_32(lv_color_white(), LV_OPA_COVER));
    lv_canvas_set_palette(context->canvas, 1,
                          lv_color_to_32(lv_color_black(), LV_OPA_COVER));

    const size_t palette_bytes = 2U * sizeof(lv_color32_t);
    uint8_t *pixels = draw_buffer->data + palette_bytes;
    memset(pixels, 0, draw_buffer->header.stride * UI_QR_CANVAS_SIZE);

    const int rendered_modules = total_modules * scale;
    const int origin =
        (((int)UI_QR_CANVAS_SIZE - rendered_modules) / 2) +
        ((int)UI_QR_QUIET_ZONE_MODULES * scale);
    for (int module_y = 0; module_y < qr_modules; ++module_y) {
        for (int module_x = 0; module_x < qr_modules; ++module_x) {
            if (!esp_qrcode_get_module(qrcode, module_x, module_y)) {
                continue;
            }
            const int pixel_x = origin + (module_x * scale);
            const int pixel_y = origin + (module_y * scale);
            for (int dy = 0; dy < scale; ++dy) {
                uint8_t *row = pixels +
                               ((pixel_y + dy) * draw_buffer->header.stride);
                for (int dx = 0; dx < scale; ++dx) {
                    const int x = pixel_x + dx;
                    row[x >> 3] |= (uint8_t)(1U << (7 - (x & 7)));
                }
            }
        }
    }

    lv_draw_buf_flush_cache(draw_buffer, NULL);
    lv_obj_invalidate(context->canvas);
    context->rendered = true;
}

static lv_color_t result_color(ui_diagnostic_result_t result)
{
    switch (result) {
    case UI_DIAGNOSTIC_OK:
        return lv_color_hex(0x8BE36D);
    case UI_DIAGNOSTIC_WARNING:
        return lv_color_hex(0xFFB454);
    case UI_DIAGNOSTIC_ERROR:
        return lv_color_hex(0xFF647C);
    case UI_DIAGNOSTIC_PENDING:
    default:
        return lv_color_hex(0x7F8FA8);
    }
}

static const char *result_text(ui_diagnostic_result_t result)
{
    switch (result) {
    case UI_DIAGNOSTIC_OK:
        return "READY";
    case UI_DIAGNOSTIC_WARNING:
        return "OPTIONAL";
    case UI_DIAGNOSTIC_ERROR:
        return "FAILED";
    case UI_DIAGNOSTIC_PENDING:
    default:
        return "CHECKING";
    }
}

static const char *compact_result_text(ui_diagnostic_result_t result)
{
    switch (result) {
    case UI_DIAGNOSTIC_OK:
        return "OK";
    case UI_DIAGNOSTIC_WARNING:
        return "OPT";
    case UI_DIAGNOSTIC_ERROR:
        return "FAIL";
    case UI_DIAGNOSTIC_PENDING:
    default:
        return "WAIT";
    }
}

static bool lcd_color_done(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *event_data,
                           void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;

    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t waiter = (TaskHandle_t)s_flush_waiter;
    if (waiter != NULL) {
        vTaskNotifyGiveIndexedFromISR(waiter, UI_FLUSH_NOTIFY_INDEX,
                                      &higher_priority_task_woken);
    }
    return higher_priority_task_woken == pdTRUE;
}

static void display_flush(lv_display_t *display, const lv_area_t *area,
                          uint8_t *color_map)
{
    if (!board_spi_acquire(pdMS_TO_TICKS(UI_FLUSH_TIMEOUT_MS))) {
        ESP_LOGE(TAG, "Timed out acquiring shared SPI2 bus");
        lv_display_flush_ready(display);
        return;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTakeIndexed(UI_FLUSH_NOTIFY_INDEX, pdTRUE, 0);
    s_flush_waiter = current_task;

    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_ui.panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
        color_map);
    if (err == ESP_OK) {
        /* A queued DMA transfer owns color_map until its ISR fires.  There is
         * no safe public abort path, so never time out and let LVGL reuse it. */
        (void)ulTaskNotifyTakeIndexed(UI_FLUSH_NOTIFY_INDEX, pdTRUE,
                                     portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "LCD draw failed: %s", esp_err_to_name(err));
    }

    s_flush_waiter = NULL;
    lv_display_flush_ready(display);
    board_spi_release();
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int32_t x,
                              int32_t y, int32_t width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static void create_status_row(lv_obj_t *screen, const char *name, int32_t y,
                              lv_obj_t **value)
{
    lv_obj_t *line = lv_obj_create(screen);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 148, 34);
    lv_obj_set_pos(line, 12, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x121E31), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 5, 0);

    create_label(line, name, 8, 9, 58, lv_color_hex(0xA7B5CA));
    *value = create_label(line, "CHECKING", 69, 9, 71,
                          result_color(UI_DIAGNOSTIC_PENDING));
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
}

static lv_obj_t *create_font_label(lv_obj_t *parent, const char *text,
                                   int32_t x, int32_t y, int32_t width,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = create_label(parent, text, x, y, width,
                                   lv_color_hex(color));
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static lv_obj_t *create_centered_label(lv_obj_t *parent, const char *text,
                                       int32_t y, const lv_font_t *font,
                                       uint32_t color)
{
    lv_obj_t *label = create_font_label(parent, text, 6, y,
                                        (int32_t)BOARD_LCD_H_RES - 12, font,
                                        color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height, uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    return panel;
}

static lv_obj_t *create_group(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(group, width, height);
    lv_obj_set_pos(group, x, y);
    return group;
}

static lv_obj_t *create_hline(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, uint32_t color)
{
    return create_panel(parent, x, y, width, 1, color);
}

static lv_obj_t *create_circle(lv_obj_t *parent, int32_t cx, int32_t cy,
                               int32_t radius, uint32_t border_color,
                               lv_opa_t border_opa, int32_t border_width)
{
    lv_obj_t *circle = create_group(parent, cx - radius, cy - radius,
                                    radius * 2, radius * 2);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(circle, border_width, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_opa(circle, border_opa, 0);
    return circle;
}

/*
 * Shared header bar: Wi-Fi glyph and status dot on the left, wordmark in the
 * middle, and a short right-hand value (target age on the tracking screen).
 */
static void create_header(lv_obj_t *screen, uint32_t accent, const char *right)
{
    lv_obj_t *bar = create_panel(screen, 0, 0, BOARD_LCD_H_RES, 24,
                                 UI_COLOR_PANEL);
    s_ui.hdr_wifi = create_font_label(bar, LV_SYMBOL_WIFI, 8, 6, 16,
                                      &lv_font_montserrat_12, accent);
    s_ui.hdr_dot = create_panel(bar, 27, 8, 8, 8, accent);
    lv_obj_set_style_radius(s_ui.hdr_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *title = create_font_label(bar, "AIRTRACK", 40, 5, 92,
                                        &lv_font_montserrat_14,
                                        UI_COLOR_TEXT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.hdr_right = create_font_label(bar, right != NULL ? right : "", 128,
                                       6, 38, &lv_font_montserrat_12,
                                       UI_COLOR_GREEN);
    lv_obj_set_style_text_align(s_ui.hdr_right, LV_TEXT_ALIGN_RIGHT, 0);
    create_hline(screen, 0, 24, BOARD_LCD_H_RES, 0x1C2A3D);
}

static esp_err_t create_setup_screen(const char *ap_ssid,
                                     const char *ap_password,
                                     const char *ip_address,
                                     bool recovery,
                                     lv_obj_t **screen_out,
                                     lv_obj_t **canvas_out,
                                     lv_draw_buf_t **draw_buffer_out)
{
    lv_draw_buf_t *qr_draw_buffer = lv_draw_buf_create(
        UI_QR_CANVAS_SIZE, UI_QR_CANVAS_SIZE, LV_COLOR_FORMAT_I1,
        LV_STRIDE_AUTO);
    if (qr_draw_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) {
        lv_draw_buf_destroy(qr_draw_buffer);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    create_header(screen, recovery ? UI_COLOR_RED : UI_COLOR_AMBER, NULL);

    create_centered_label(screen,
                          recovery ? "WI-FI LOST" : "SCAN TO CONNECT", 32,
                          &lv_font_montserrat_16,
                          recovery ? UI_COLOR_RED : UI_COLOR_AMBER);

    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(canvas, qr_draw_buffer);
    lv_obj_set_size(canvas, UI_QR_CANVAS_SIZE, UI_QR_CANVAS_SIZE);
    lv_obj_set_pos(canvas, ((int32_t)BOARD_LCD_H_RES - UI_QR_CANVAS_SIZE) / 2,
                   56);

    lv_obj_t *ssid = create_centered_label(screen, ap_ssid, 202,
                                           &lv_font_montserrat_20,
                                           UI_COLOR_AMBER);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(ssid, 5000, 0);

    /* "Password:" in white and the secret in amber, centred as one line. */
    static const char password_caption[] = "Password: ";
    lv_point_t caption_size;
    lv_point_t value_size;
    lv_text_get_size(&caption_size, password_caption, &lv_font_montserrat_14,
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&value_size, ap_password, &lv_font_montserrat_14, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t total = caption_size.x + value_size.x;
    int32_t max_total = (int32_t)BOARD_LCD_H_RES - 12;
    if (total > max_total) {
        total = max_total;
    }
    const int32_t start = ((int32_t)BOARD_LCD_H_RES - total) / 2;
    create_font_label(screen, password_caption, start, 232, caption_size.x + 2,
                      &lv_font_montserrat_14, UI_COLOR_TEXT);
    create_font_label(screen, ap_password, start + caption_size.x, 232,
                      max_total - caption_size.x - (start - 6),
                      &lv_font_montserrat_14, UI_COLOR_AMBER);

    create_centered_label(screen, ip_address, 254, &lv_font_montserrat_16,
                          UI_COLOR_AMBER);

    create_centered_label(
        screen,
        recovery ? "Retrying Wi-Fi " LV_SYMBOL_BULLET " or scan the QR"
                 : "Join, then open the address",
        282, &lv_font_montserrat_10, UI_COLOR_DIM);
    create_hline(screen, 14, 298, BOARD_LCD_H_RES - 28, 0x1C2A3D);
    create_centered_label(screen, "Data: adsb.fi", 304,
                          &lv_font_montserrat_10, UI_COLOR_MUTED);

    *screen_out = screen;
    *canvas_out = canvas;
    *draw_buffer_out = qr_draw_buffer;
    return ESP_OK;
}

static void create_screen(void)
{
    s_ui.screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, BOARD_LCD_H_RES, 30);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    create_label(header, "AIRTRACK", 10, 8, 86, lv_color_hex(0x55D9F3));
    lv_obj_t *mode = create_label(header, "HW", 97, 8, 65,
                                  lv_color_hex(0x8BE36D));
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_RIGHT, 0);

    create_label(s_ui.screen, "SYSTEM BRING-UP", 12, 43, 148,
                 lv_color_hex(0x6F819B));
    s_ui.phase = create_label(s_ui.screen, "Starting hardware...", 12, 64, 148,
                              lv_color_hex(0xF2F6FC));

    create_status_row(s_ui.screen, "LCD", 94, &s_ui.lcd_value);
    create_status_row(s_ui.screen, "SD", 134, &s_ui.sd_value);
    create_status_row(s_ui.screen, "FLASH", 174, &s_ui.flash_value);

    lv_obj_t *footer = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, BOARD_LCD_H_RES, 94);
    lv_obj_set_pos(footer, 0, 226);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    create_label(footer, "NETWORK", 10, 9, 152, lv_color_hex(0x6F819B));
    s_ui.ssid = create_label(footer, "SSID: --", 10, 31, 152,
                             lv_color_hex(0xDDE7F4));
    s_ui.ip = create_label(footer, "IP: --", 10, 51, 152,
                           lv_color_hex(0xDDE7F4));
    create_label(footer, "Data: adsb.fi", 10, 72, 152,
                 lv_color_hex(0x55D9F3));

    lv_screen_load(s_ui.screen);
}

esp_err_t ui_diagnostic_init(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_handle_t panel)
{
    if (s_ui.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (io == NULL || panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const lvgl_port_cfg_t port_config = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 250,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&port_config);
    if (err != ESP_OK) {
        return err;
    }
    s_ui.port_started = true;
    s_ui.panel = panel;

    const size_t draw_buffer_bytes =
        BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * sizeof(lv_color16_t);
    const uint32_t draw_caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    s_ui.draw_buffer_a = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_buffer_bytes, draw_caps);
    s_ui.draw_buffer_b = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_buffer_bytes, draw_caps);
    if (s_ui.draw_buffer_a == NULL || s_ui.draw_buffer_b == NULL) {
        heap_caps_free(s_ui.draw_buffer_a);
        heap_caps_free(s_ui.draw_buffer_b);
        s_ui.draw_buffer_a = NULL;
        s_ui.draw_buffer_b = NULL;
        (void)lvgl_port_deinit();
        s_ui.port_started = false;
        s_ui.panel = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Register the completion callback before a display exists.  The helper
     * add-display API installs its own callback after creating a refresh timer;
     * replacing that callback can strand its first in-flight frame.  Owning the
     * display and buffers here keeps the callback/flush pair atomic. */
    err = board_lcd_register_color_done_callback(lcd_color_done, NULL);
    if (err != ESP_OK) {
        heap_caps_free(s_ui.draw_buffer_a);
        heap_caps_free(s_ui.draw_buffer_b);
        s_ui.draw_buffer_a = NULL;
        s_ui.draw_buffer_b = NULL;
        (void)lvgl_port_deinit();
        s_ui.port_started = false;
        s_ui.panel = NULL;
        return err;
    }
    s_ui.callback_registered = true;

    if (!lvgl_port_lock(1000)) {
        (void)board_lcd_register_color_done_callback(NULL, NULL);
        s_ui.callback_registered = false;
        heap_caps_free(s_ui.draw_buffer_a);
        heap_caps_free(s_ui.draw_buffer_b);
        s_ui.draw_buffer_a = NULL;
        s_ui.draw_buffer_b = NULL;
        (void)lvgl_port_deinit();
        s_ui.port_started = false;
        s_ui.panel = NULL;
        return ESP_ERR_TIMEOUT;
    }

    s_ui.display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (s_ui.display == NULL) {
        lvgl_port_unlock();
        (void)board_lcd_register_color_done_callback(NULL, NULL);
        s_ui.callback_registered = false;
        heap_caps_free(s_ui.draw_buffer_a);
        heap_caps_free(s_ui.draw_buffer_b);
        s_ui.draw_buffer_a = NULL;
        s_ui.draw_buffer_b = NULL;
        (void)lvgl_port_deinit();
        s_ui.port_started = false;
        s_ui.panel = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_default(s_ui.display);
    lv_display_set_color_format(s_ui.display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_ui.display, s_ui.draw_buffer_a,
                           s_ui.draw_buffer_b, draw_buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_ui.display, display_flush);
    create_screen();
    s_ui.diagnostic_visible = true;
    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);

    s_ui.initialized = true;
    ESP_LOGI(TAG, "LVGL diagnostic UI ready (%ux%u, two %u-line DMA buffers)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_STRIP_LINES);
    return ESP_OK;
}

esp_err_t ui_diagnostic_update(const ui_diagnostic_state_t *state)
{
    if (!s_ui.initialized || !s_ui.diagnostic_visible) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_label_set_text(s_ui.phase,
                      state->phase != NULL ? state->phase : "Hardware ready");

    lv_label_set_text(s_ui.lcd_value, result_text(state->lcd));
    lv_obj_set_style_text_color(s_ui.lcd_value, result_color(state->lcd), 0);

    lv_label_set_text(s_ui.sd_value, result_text(state->sd));
    lv_obj_set_style_text_color(s_ui.sd_value, result_color(state->sd), 0);

    char flash_text[24];
    if (state->flash_bytes > 0) {
        (void)snprintf(flash_text, sizeof(flash_text), "%luMB %s",
                       (unsigned long)(state->flash_bytes / (1024U * 1024U)),
                       compact_result_text(state->flash));
    } else {
        (void)snprintf(flash_text, sizeof(flash_text), "%s",
                       result_text(state->flash));
    }
    lv_label_set_text(s_ui.flash_value, flash_text);
    lv_obj_set_style_text_color(s_ui.flash_value, result_color(state->flash), 0);

    char network_text[96];
    (void)snprintf(network_text, sizeof(network_text), "SSID: %s",
                   state->ssid != NULL && state->ssid[0] != '\0' ? state->ssid : "--");
    lv_label_set_text(s_ui.ssid, network_text);
    (void)snprintf(network_text, sizeof(network_text), "IP: %s",
                   state->ip_address != NULL && state->ip_address[0] != '\0'
                       ? state->ip_address
                       : "--");
    lv_label_set_text(s_ui.ip, network_text);

    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    return ESP_OK;
}

esp_err_t ui_diagnostic_show_setup(const char *ap_ssid,
                                   const char *ap_password,
                                   const char *ip_address,
                                   bool recovery)
{
    if (!s_ui.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!validate_printable_bytes(ap_ssid, 1U, UI_WIFI_SSID_MAX_BYTES) ||
        !validate_printable_bytes(ap_password, UI_WIFI_PASSWORD_MIN_BYTES,
                                  UI_WIFI_PASSWORD_MAX_BYTES) ||
        !validate_ipv4_address(ip_address)) {
        return ESP_ERR_INVALID_ARG;
    }

    char escaped_ssid[(UI_WIFI_SSID_MAX_BYTES * 2U) + 1U] = {0};
    char escaped_password[(UI_WIFI_PASSWORD_MAX_BYTES * 2U) + 1U] = {0};
    char payload[UI_WIFI_QR_PAYLOAD_BYTES] = {0};
    if (!wifi_qr_escape(ap_ssid, escaped_ssid, sizeof(escaped_ssid)) ||
        !wifi_qr_escape(ap_password, escaped_password,
                        sizeof(escaped_password))) {
        clear_sensitive_buffer(escaped_password, sizeof(escaped_password));
        return ESP_ERR_INVALID_SIZE;
    }

    const int payload_length = snprintf(
        payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;H:false;;",
        escaped_ssid, escaped_password);
    clear_sensitive_buffer(escaped_password, sizeof(escaped_password));
    if (payload_length < 0 || (size_t)payload_length >= sizeof(payload)) {
        clear_sensitive_buffer(payload, sizeof(payload));
        return ESP_ERR_INVALID_SIZE;
    }

    if (!lvgl_port_lock(1000)) {
        clear_sensitive_buffer(payload, sizeof(payload));
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *new_screen = NULL;
    lv_obj_t *qr_canvas = NULL;
    lv_draw_buf_t *new_qr_draw_buffer = NULL;
    esp_err_t err = create_setup_screen(ap_ssid, ap_password, ip_address,
                                        recovery, &new_screen, &qr_canvas,
                                        &new_qr_draw_buffer);
    if (err == ESP_OK) {
        qr_render_context_t render_context = {
            .canvas = qr_canvas,
        };
        esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
        qr_config.display_func_with_cb = render_qr_to_canvas;
        qr_config.user_data = &render_context;
        qr_config.max_qrcode_version = UI_QR_MAX_VERSION;
        qr_config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;

        /* qrcode 0.2 logs the full encoded text at INFO.  Disable its tag
         * while credentials are present so the AP password never reaches the
         * serial console. */
        const esp_log_level_t previous_qr_log_level =
            esp_log_level_get("QRCODE");
        esp_log_level_set("QRCODE", ESP_LOG_NONE);
        err = esp_qrcode_generate(&qr_config, payload);
        esp_log_level_set("QRCODE", previous_qr_log_level);
        if (err == ESP_OK && !render_context.rendered) {
            err = ESP_FAIL;
        }
    }
    clear_sensitive_buffer(payload, sizeof(payload));

    if (err != ESP_OK) {
        if (new_screen != NULL) {
            lv_obj_delete(new_screen);
        }
        if (new_qr_draw_buffer != NULL) {
            lv_draw_buf_destroy(new_qr_draw_buffer);
        }
        lvgl_port_unlock();
        return err;
    }

    lv_obj_t *old_screen = s_ui.screen;
    lv_draw_buf_t *old_qr_draw_buffer = s_ui.qr_draw_buffer;
    lv_screen_load(new_screen);
    s_ui.screen = new_screen;
    s_ui.qr_draw_buffer = new_qr_draw_buffer;
    s_ui.phase = NULL;
    s_ui.lcd_value = NULL;
    s_ui.sd_value = NULL;
    s_ui.flash_value = NULL;
    s_ui.ssid = NULL;
    s_ui.ip = NULL;
    s_ui.diagnostic_visible = false;
    if (s_ui.tracking_visible && s_ui.radar_animating) {
        lv_anim_delete(s_ui.trk_radar_sweep, radar_sweep_animate);
        s_ui.radar_animating = false;
    }
    s_ui.tracking_visible = false;
    if (old_screen != NULL) {
        lv_obj_delete(old_screen);
    }
    if (old_qr_draw_buffer != NULL) {
        lv_draw_buf_destroy(old_qr_draw_buffer);
    }

    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    ESP_LOGI(TAG, "Setup UI ready for AP %s at %s", ap_ssid, ip_address);
    return ESP_OK;
}

static lv_color_t tracking_state_color(airtrack_feed_state_t state)
{
    switch (state) {
    case AIRTRACK_FEED_LIVE:
        return lv_color_hex(UI_COLOR_GREEN);
    case AIRTRACK_FEED_EMPTY:
    case AIRTRACK_FEED_SEARCHING:
    case AIRTRACK_FEED_TIME_SYNC:
        return lv_color_hex(UI_COLOR_CYAN);
    case AIRTRACK_FEED_STALE:
    case AIRTRACK_FEED_CONFIG_REQUIRED:
        return lv_color_hex(UI_COLOR_AMBER);
    case AIRTRACK_FEED_OFFLINE:
    default:
        return lv_color_hex(UI_COLOR_RED);
    }
}

static const char *target_identity(const airtrack_aircraft_t *aircraft)
{
    return aircraft->callsign[0] != '\0' ? aircraft->callsign
           : aircraft->registration[0] != '\0' ? aircraft->registration
                                                : aircraft->hex;
}

static const char *cardinal_name(float bearing_deg)
{
    static const char *names[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    float normalized = bearing_deg;
    while (normalized < 0.0f) {
        normalized += 360.0f;
    }
    while (normalized >= 360.0f) {
        normalized -= 360.0f;
    }
    const unsigned index = (unsigned)((normalized + 11.25f) / 22.5f) % 16U;
    return names[index];
}

/* Format a nonnegative integer with thousands separators, e.g. "18,325". */
static void format_grouped(char *out, size_t capacity, long value)
{
    char digits[16];
    const int length = snprintf(digits, sizeof(digits), "%ld",
                                value < 0 ? -value : value);
    if (length < 0 || (size_t)length >= sizeof(digits) || capacity < 2U) {
        if (capacity > 0U) {
            out[0] = '\0';
        }
        return;
    }
    size_t used = 0U;
    if (value < 0 && used + 1U < capacity) {
        out[used++] = '-';
    }
    for (int index = 0; index < length && used + 1U < capacity; ++index) {
        out[used++] = digits[index];
        const int remaining = length - index - 1;
        if (remaining > 0 && remaining % 3 == 0 && used + 1U < capacity) {
            out[used++] = ',';
        }
    }
    out[used] = '\0';
}

static const char *unit_name(airtrack_distance_unit_t unit)
{
    return unit == AIRTRACK_DISTANCE_KM ? "km"
           : unit == AIRTRACK_DISTANCE_MI ? "mi" : "NM";
}

static float unit_scale(airtrack_distance_unit_t unit)
{
    return unit == AIRTRACK_DISTANCE_KM ? 1.852f
           : unit == AIRTRACK_DISTANCE_MI ? 1.150779f : 1.0f;
}

/* Compass geometry (screen coordinates inside the data group). */
#define UI_COMPASS_CX 86
#define UI_COMPASS_CY 140
#define UI_COMPASS_R 38

static void set_arrow_bearing(float bearing_deg)
{
    const float radians = bearing_deg * (3.14159265f / 180.0f);
    const float cx = UI_COMPASS_CX;
    const float cy = UI_COMPASS_CY;
    const float tip = UI_COMPASS_R - 5.0f;
    const float tail = 15.0f;
    const float head = 8.0f;
    const float head_angle = 150.0f * (3.14159265f / 180.0f);
    lv_point_precise_t *points = s_ui.arrow_points;
    const float tip_x = cx + tip * sinf(radians);
    const float tip_y = cy - tip * cosf(radians);
    points[0].x = cx + tail * sinf(radians);
    points[0].y = cy - tail * cosf(radians);
    points[1].x = tip_x;
    points[1].y = tip_y;
    points[2].x = tip_x + head * sinf(radians - head_angle);
    points[2].y = tip_y - head * cosf(radians - head_angle);
    points[3] = points[1];
    points[4].x = tip_x + head * sinf(radians + head_angle);
    points[4].y = tip_y - head * cosf(radians + head_angle);
    lv_line_set_points(s_ui.trk_arrow, s_ui.arrow_points, 5);

    /* Highlight arc: LVGL arcs start at 3 o'clock, compass at 12. */
    int32_t start = (int32_t)bearing_deg - 90 - 14;
    while (start < 0) {
        start += 360;
    }
    lv_arc_set_angles(s_ui.trk_arc, start, start + 28);
}

static void create_compass(lv_obj_t *parent)
{
    static const char *cardinals[] = {"N", "E", "S", "W", "N", NULL};

    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_remove_style_all(scale);
    lv_obj_remove_flag(scale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scale, UI_COMPASS_R * 2, UI_COMPASS_R * 2);
    lv_obj_set_pos(scale, UI_COMPASS_CX - UI_COMPASS_R,
                   UI_COMPASS_CY - UI_COMPASS_R);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(scale, 0, 360);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);
    lv_scale_set_total_tick_count(scale, 13);
    lv_scale_set_major_tick_every(scale, 3);
    lv_scale_set_label_show(scale, true);
    lv_scale_set_text_src(scale, cardinals);
    /* ring */
    lv_obj_set_style_arc_width(scale, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(scale, lv_color_hex(0x2F415C), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(scale, LV_OPA_COVER, LV_PART_MAIN);
    /* major ticks + labels */
    lv_obj_set_style_length(scale, 5, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x5B6F8F), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(scale, &lv_font_montserrat_10, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_hex(UI_COLOR_DIM), LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(scale, 0, LV_PART_INDICATOR);
    /* minor ticks */
    lv_obj_set_style_length(scale, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x3D5170), LV_PART_ITEMS);
    s_ui.trk_scale = scale;

    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, UI_COMPASS_R * 2, UI_COMPASS_R * 2);
    lv_obj_set_pos(arc, UI_COMPASS_CX - UI_COMPASS_R,
                   UI_COMPASS_CY - UI_COMPASS_R);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    s_ui.trk_arc = arc;

    lv_obj_t *plane = lv_image_create(parent);
    lv_image_set_src(plane, &ui_icon_plane);
    lv_obj_set_pos(plane, UI_COMPASS_CX - 14, UI_COMPASS_CY - 14);
    lv_obj_set_style_image_recolor(plane, lv_color_hex(UI_COLOR_CYAN), 0);
    lv_obj_set_style_image_recolor_opa(plane, LV_OPA_COVER, 0);
    lv_image_set_pivot(plane, 14, 14);
    lv_image_set_scale(plane, 190); /* 28 px source drawn at ~21 px */
    s_ui.trk_plane = plane;

    lv_obj_t *arrow = lv_line_create(parent);
    lv_obj_remove_style_all(arrow);
    lv_obj_set_pos(arrow, 0, 0);
    lv_obj_set_size(arrow, BOARD_LCD_H_RES, UI_COMPASS_CY + UI_COMPASS_R + 2);
    lv_obj_set_style_line_width(arrow, 2, 0);
    lv_obj_set_style_line_rounded(arrow, true, 0);
    lv_obj_set_style_line_color(arrow, lv_color_hex(UI_COLOR_CYAN), 0);
    s_ui.trk_arrow = arrow;
    set_arrow_bearing(0.0f);

    /* Route codes flank the gauge, one letter per line. */
    const int32_t top = UI_COMPASS_CY - UI_COMPASS_R;
    s_ui.trk_from_caption = create_font_label(parent, "", 4, top, 40,
                                              &lv_font_montserrat_10,
                                              UI_COLOR_MUTED);
    lv_obj_set_style_text_align(s_ui.trk_from_caption, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_from = create_font_label(parent, "", 4, top + 12, 40,
                                      &lv_font_montserrat_14, UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.trk_from, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_ui.trk_from, -1, 0);
    s_ui.trk_to_caption = create_font_label(parent, "", BOARD_LCD_H_RES - 44,
                                            top, 40, &lv_font_montserrat_10,
                                            UI_COLOR_MUTED);
    lv_obj_set_style_text_align(s_ui.trk_to_caption, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_to = create_font_label(parent, "", BOARD_LCD_H_RES - 44, top + 12,
                                    40, &lv_font_montserrat_14, UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.trk_to, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_ui.trk_to, -1, 0);
}

/* "SEA" -> "S\nE\nA" for the vertical route columns. */
static void vertical_code(char *out, size_t capacity, const char *code)
{
    size_t used = 0U;
    for (size_t index = 0U; code[index] != '\0' && used + 2U < capacity; ++index) {
        if (index > 0U) {
            out[used++] = '\n';
        }
        out[used++] = code[index];
    }
    out[used] = '\0';
}

static const char *empty_focus_headline(char *buffer, size_t capacity,
                                        const char *focus)
{
    (void)snprintf(buffer, capacity, "WAITING FOR\n%s", focus);
    return buffer;
}

static void create_data_row(lv_obj_t *parent, size_t index, int32_t y,
                            const lv_image_dsc_t *icon, const char *symbol)
{
    if (index > 0U) {
        create_hline(parent, 14, y - 3, BOARD_LCD_H_RES - 28, 0x1C2A3D);
    }
    if (icon != NULL) {
        lv_obj_t *image = lv_image_create(parent);
        lv_image_set_src(image, icon);
        lv_obj_set_pos(image, 22, y + 1);
        lv_obj_set_style_image_recolor(image, lv_color_hex(UI_COLOR_CYAN), 0);
        lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
        s_ui.trk_row_icon[index] = image;
    } else {
        s_ui.trk_row_icon[index] = create_font_label(
            parent, symbol, 22, y + 1, 18, &lv_font_montserrat_14,
            UI_COLOR_CYAN);
    }
    s_ui.trk_row_text[index] = create_font_label(
        parent, "--", 46, y, BOARD_LCD_H_RES - 50, &lv_font_montserrat_14,
        UI_COLOR_TEXT);
    /* One line only; never wrap into the row below or the footer. */
    lv_label_set_long_mode(s_ui.trk_row_text[index], LV_LABEL_LONG_CLIP);
}

static void create_radar(lv_obj_t *parent, int32_t cx, int32_t cy)
{
    static const int32_t radii[4] = {14, 28, 42, 56};
    static const lv_opa_t opas[4] = {150, 110, 80, 60};
    for (size_t index = 0U; index < 4U; ++index) {
        s_ui.trk_radar_ring[index] = create_circle(
            parent, cx, cy, radii[index], UI_COLOR_CYAN, opas[index], 1);
    }
    lv_obj_t *sweep = lv_arc_create(parent);
    lv_obj_remove_style_all(sweep);
    lv_obj_remove_flag(sweep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sweep, 112, 112);
    lv_obj_set_pos(sweep, cx - 56, cy - 56);
    lv_arc_set_bg_angles(sweep, 0, 360);
    lv_arc_set_angles(sweep, 300, 340);
    lv_obj_set_style_arc_opa(sweep, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sweep, 56, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sweep, lv_color_hex(UI_COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(sweep, 70, LV_PART_INDICATOR);
    s_ui.trk_radar_sweep = sweep;

    s_ui.trk_radar_dot = create_panel(parent, cx - 4, cy - 4, 8, 8, UI_COLOR_CYAN);
    lv_obj_set_style_radius(s_ui.trk_radar_dot, LV_RADIUS_CIRCLE, 0);
}

static void radar_sweep_animate(void *object, int32_t value)
{
    lv_arc_set_angles(object, value, value + 40);
}

static void set_radar_animation(bool enabled)
{
    if (enabled == s_ui.radar_animating) {
        return;
    }
    s_ui.radar_animating = enabled;
    if (!enabled) {
        lv_anim_delete(s_ui.trk_radar_sweep, radar_sweep_animate);
        lv_arc_set_angles(s_ui.trk_radar_sweep, 300, 340);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_ui.trk_radar_sweep);
    lv_anim_set_exec_cb(&anim, radar_sweep_animate);
    lv_anim_set_values(&anim, 0, 359);
    lv_anim_set_duration(&anim, UI_RADAR_SWEEP_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

static void create_tracking_screen_locked(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    create_header(screen, UI_COLOR_GREEN, "");

    /* Target block. */
    s_ui.trk_data = create_group(screen, 0, 24, BOARD_LCD_H_RES, 272);
    s_ui.trk_identity = create_centered_label(s_ui.trk_data, "--", 3,
                                              &lv_font_montserrat_28,
                                              UI_COLOR_TEXT);
    s_ui.trk_meta = create_centered_label(s_ui.trk_data, "", 37,
                                          &lv_font_montserrat_12,
                                          UI_COLOR_DIM);
    s_ui.trk_divider = create_hline(s_ui.trk_data, 14, 54,
                                    BOARD_LCD_H_RES - 28, UI_COLOR_CYAN);
    s_ui.trk_distance = create_font_label(s_ui.trk_data, "--", 0, 55, 120,
                                          &lv_font_montserrat_40,
                                          UI_COLOR_CYAN);
    s_ui.trk_unit = create_font_label(s_ui.trk_data, "NM", 120, 73, 46,
                                      &lv_font_montserrat_20, UI_COLOR_CYAN);
    create_compass(s_ui.trk_data);
    create_data_row(s_ui.trk_data, 0, 183, &ui_icon_nav, NULL);
    create_data_row(s_ui.trk_data, 1, 205, &ui_icon_mountain, NULL);
    create_data_row(s_ui.trk_data, 2, 227, NULL, LV_SYMBOL_UP);
    create_data_row(s_ui.trk_data, 3, 249, &ui_icon_gauge, NULL);

    /* Empty / waiting block. */
    s_ui.trk_empty = create_group(screen, 0, 24, BOARD_LCD_H_RES, 272);
    s_ui.trk_empty_head = create_centered_label(s_ui.trk_empty,
                                                "NO RECENT\nREPORTS", 12,
                                                &lv_font_montserrat_20,
                                                UI_COLOR_CYAN);
    create_radar(s_ui.trk_empty, 86, 126);
    s_ui.trk_empty_within = create_centered_label(s_ui.trk_empty, "within",
                                                  188, &lv_font_montserrat_14,
                                                  UI_COLOR_TEXT);
    s_ui.trk_empty_radius = create_font_label(s_ui.trk_empty, "25", 0, 206,
                                              110, &lv_font_montserrat_28,
                                              UI_COLOR_CYAN);
    s_ui.trk_empty_unit = create_font_label(s_ui.trk_empty, "NM", 110, 218,
                                            50, &lv_font_montserrat_16,
                                            UI_COLOR_CYAN);
    s_ui.trk_empty_hint = create_centered_label(s_ui.trk_empty, "", 246,
                                                &lv_font_montserrat_12,
                                                UI_COLOR_MUTED);
    lv_obj_add_flag(s_ui.trk_empty, LV_OBJ_FLAG_HIDDEN);

    /* Footer. */
    create_hline(screen, 14, 296, BOARD_LCD_H_RES - 28, 0x1C2A3D);
    s_ui.trk_footer_net = create_centered_label(screen, "", 299,
                                                &lv_font_montserrat_10,
                                                UI_COLOR_DIM);
    s_ui.trk_footer_data = create_centered_label(screen, "Data: adsb.fi", 309,
                                                 &lv_font_montserrat_10,
                                                 UI_COLOR_MUTED);

    lv_obj_t *old_screen = s_ui.screen;
    lv_draw_buf_t *old_qr = s_ui.qr_draw_buffer;
    lv_screen_load(screen);
    s_ui.screen = screen;
    s_ui.qr_draw_buffer = NULL;
    if (old_screen != NULL) {
        lv_obj_delete(old_screen);
    }
    if (old_qr != NULL) {
        lv_draw_buf_destroy(old_qr);
    }
    s_ui.phase = NULL;
    s_ui.lcd_value = NULL;
    s_ui.sd_value = NULL;
    s_ui.flash_value = NULL;
    s_ui.ssid = NULL;
    s_ui.ip = NULL;
    s_ui.radar_animating = false;
    s_ui.diagnostic_visible = false;
    s_ui.tracking_visible = true;
}

static void set_label_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void show_group(lv_obj_t *group, bool visible)
{
    const bool hidden = lv_obj_has_flag(group, LV_OBJ_FLAG_HIDDEN);
    if (visible && hidden) {
        lv_obj_remove_flag(group, LV_OBJ_FLAG_HIDDEN);
    } else if (!visible && !hidden) {
        lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Centre a "<number><unit>" pair: number in a large font, unit smaller. */
static void layout_value_pair(lv_obj_t *number, const lv_font_t *number_font,
                              lv_obj_t *unit, const lv_font_t *unit_font,
                              int32_t y, int32_t unit_offset_y)
{
    lv_point_t number_size;
    lv_point_t unit_size;
    lv_text_get_size(&number_size, lv_label_get_text(number), number_font, 0,
                     0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&unit_size, lv_label_get_text(unit), unit_font, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t gap = 6;
    int32_t total = number_size.x + gap + unit_size.x;
    const int32_t max_total = (int32_t)BOARD_LCD_H_RES - 12;
    if (total > max_total) {
        total = max_total;
    }
    const int32_t start = ((int32_t)BOARD_LCD_H_RES - total) / 2;
    lv_obj_set_pos(number, start, y);
    lv_obj_set_width(number, number_size.x + 2);
    lv_obj_set_pos(unit, start + number_size.x + gap, y + unit_offset_y);
    lv_obj_set_width(unit, unit_size.x + 2);
}

static const char *empty_headline(const ui_tracking_state_t *state)
{
    switch (state->snapshot->state) {
    case AIRTRACK_FEED_EMPTY:
        return "NO RECENT\nREPORTS";
    case AIRTRACK_FEED_TIME_SYNC:
        return "SYNCING\nTIME";
    case AIRTRACK_FEED_CONFIG_REQUIRED:
        return "SET\nLOCATION";
    case AIRTRACK_FEED_OFFLINE:
        return state->wifi_connected ? "FEED\nOFFLINE" : "NO\nWI-FI";
    default:
        return "SEARCHING";
    }
}

esp_err_t ui_diagnostic_show_tracking(const ui_tracking_state_t *state)
{
    if (!s_ui.initialized || state == NULL || state->settings == NULL ||
        state->snapshot == NULL || state->ssid == NULL ||
        state->ip_address == NULL ||
        airtrack_settings_validate(state->settings) != ESP_OK ||
        state->snapshot->aircraft_count > AIRTRACK_MAX_AIRCRAFT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_ui.tracking_visible) {
        create_tracking_screen_locked();
    }

    const airtrack_snapshot_t *snapshot = state->snapshot;
    const lv_color_t state_color = tracking_state_color(snapshot->state);
    char text[96];

    /* Header: Wi-Fi glyph, feed dot, and age of the last good poll. */
    lv_obj_set_style_text_color(s_ui.hdr_wifi,
        lv_color_hex(state->wifi_connected ? UI_COLOR_GREEN : UI_COLOR_RED), 0);
    lv_obj_set_style_bg_color(s_ui.hdr_dot, state_color, 0);
    double since_success = -1.0;
    if (snapshot->last_success_monotonic_ms > 0) {
        const int64_t elapsed = (esp_timer_get_time() / 1000LL) -
                                snapshot->last_success_monotonic_ms;
        since_success = elapsed > 0 ? (double)elapsed / 1000.0 : 0.0;
    }
    if (since_success < 0.0) {
        text[0] = '\0';
    } else if (since_success < 100.0) {
        (void)snprintf(text, sizeof(text), "%.0fs", since_success);
    } else if (since_success < 6000.0) {
        (void)snprintf(text, sizeof(text), "%.0fm", since_success / 60.0);
    } else {
        (void)snprintf(text, sizeof(text), "%.0fh", since_success / 3600.0);
    }
    set_label_if_changed(s_ui.hdr_right, text);
    lv_obj_set_style_text_color(s_ui.hdr_right,
        lv_color_hex(since_success >= 0.0 && since_success <= UI_STALE_AGE_S
                         ? UI_COLOR_GREEN : UI_COLOR_AMBER), 0);

    if (snapshot->aircraft_count > 0U) {
        const airtrack_aircraft_t *aircraft = &snapshot->aircraft[0];
        set_radar_animation(false);
        show_group(s_ui.trk_empty, false);
        show_group(s_ui.trk_data, true);
        double age = aircraft->seen_pos_s + (since_success > 0.0 ? since_success : 0.0);
        const bool dimmed = snapshot->state != AIRTRACK_FEED_LIVE ||
                            age > UI_STALE_AGE_S;
        const uint32_t value_color = dimmed ? UI_COLOR_DIM : UI_COLOR_TEXT;
        const uint32_t accent = aircraft->emergency ? UI_COLOR_RED
                                : dimmed ? UI_COLOR_AMBER : UI_COLOR_CYAN;
        const lv_color_t accent_color = lv_color_hex(accent);

        set_label_if_changed(s_ui.trk_identity, target_identity(aircraft));
        lv_obj_set_style_text_color(s_ui.trk_identity,
            lv_color_hex(aircraft->emergency ? UI_COLOR_RED : value_color), 0);

        /* TYPE • REGISTRATION (falling back to hex), or the emergency. */
        if (aircraft->emergency) {
            (void)snprintf(text, sizeof(text), "EMERGENCY " LV_SYMBOL_BULLET " %s",
                           aircraft->squawk[0] != '\0' ? aircraft->squawk : "");
        } else {
            const char *type = aircraft->aircraft_type;
            const char *reg = aircraft->registration[0] != '\0' &&
                              strcmp(aircraft->registration,
                                     target_identity(aircraft)) != 0
                                  ? aircraft->registration : "";
            if (type[0] != '\0' && reg[0] != '\0') {
                (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " %s",
                               type, reg);
            } else if (type[0] != '\0' || reg[0] != '\0') {
                (void)snprintf(text, sizeof(text), "%s%s", type, reg);
            } else {
                (void)snprintf(text, sizeof(text), "%s", aircraft->hex);
            }
        }
        set_label_if_changed(s_ui.trk_meta, text);
        lv_obj_set_style_text_color(s_ui.trk_meta,
            lv_color_hex(aircraft->emergency ? UI_COLOR_RED : UI_COLOR_DIM), 0);
        lv_obj_set_style_bg_color(s_ui.trk_divider, accent_color, 0);

        const float distance = aircraft->distance_nm *
                               unit_scale(state->settings->distance_unit);
        (void)snprintf(text, sizeof(text), distance < 100.0f ? "%.1f" : "%.0f",
                       (double)distance);
        set_label_if_changed(s_ui.trk_distance, text);
        set_label_if_changed(s_ui.trk_unit,
                             unit_name(state->settings->distance_unit));
        layout_value_pair(s_ui.trk_distance, &lv_font_montserrat_40,
                          s_ui.trk_unit, &lv_font_montserrat_20, 55, 18);
        lv_obj_set_style_text_color(s_ui.trk_distance, accent_color, 0);
        lv_obj_set_style_text_color(s_ui.trk_unit, accent_color, 0);

        set_arrow_bearing(aircraft->bearing_deg);
        lv_obj_set_style_line_color(s_ui.trk_arrow, accent_color, 0);
        lv_obj_set_style_arc_color(s_ui.trk_arc, accent_color, LV_PART_INDICATOR);
        lv_obj_set_style_image_recolor(s_ui.trk_plane, accent_color, 0);
        lv_image_set_rotation(s_ui.trk_plane,
            aircraft->track_valid ? (int32_t)(aircraft->track_deg * 10.0f)
                                  : (int32_t)(aircraft->bearing_deg * 10.0f));

        /* Route columns beside the gauge. */
        if (aircraft->route_valid) {
            char column[12];
            vertical_code(column, sizeof(column), aircraft->route_from);
            set_label_if_changed(s_ui.trk_from, column);
            vertical_code(column, sizeof(column), aircraft->route_to);
            set_label_if_changed(s_ui.trk_to, column);
            set_label_if_changed(s_ui.trk_from_caption, "FROM");
            set_label_if_changed(s_ui.trk_to_caption, "TO");
        } else {
            set_label_if_changed(s_ui.trk_from, "");
            set_label_if_changed(s_ui.trk_to, "");
            set_label_if_changed(s_ui.trk_from_caption, "");
            set_label_if_changed(s_ui.trk_to_caption, "");
        }
        lv_obj_set_style_text_color(s_ui.trk_from, lv_color_hex(value_color), 0);
        lv_obj_set_style_text_color(s_ui.trk_to, lv_color_hex(value_color), 0);

        /* Remaining distance / ETA to the destination when known. */
        float remaining_nm = -1.0f;
        long eta_s = -1;
        if (aircraft->destination_valid) {
            float bearing_unused;
            airtrack_geometry(aircraft->latitude, aircraft->longitude,
                              aircraft->destination_latitude,
                              aircraft->destination_longitude,
                              &remaining_nm, &bearing_unused);
            if (aircraft->ground_speed_valid && aircraft->ground_speed_kt >= 60.0f) {
                eta_s = (long)(remaining_nm / aircraft->ground_speed_kt * 3600.0f);
            }
        }
        const bool focused = state->settings->focus_flight[0] != '\0';
        if (focused && aircraft->route_valid && !aircraft->emergency) {
            if (remaining_nm >= 0.0f) {
                (void)snprintf(text, sizeof(text), "%s-%s " LV_SYMBOL_BULLET " %.0f %s to go",
                               aircraft->route_from, aircraft->route_to,
                               (double)(remaining_nm *
                                        unit_scale(state->settings->distance_unit)),
                               unit_name(state->settings->distance_unit));
            } else {
                (void)snprintf(text, sizeof(text), "%s-%s " LV_SYMBOL_BULLET " %s",
                               aircraft->route_from, aircraft->route_to,
                               aircraft->aircraft_type);
            }
            set_label_if_changed(s_ui.trk_meta, text);
        }

        (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " %03.0f°",
                       cardinal_name(aircraft->bearing_deg),
                       (double)aircraft->bearing_deg);
        set_label_if_changed(s_ui.trk_row_text[0], text);

        if (aircraft->ground) {
            (void)snprintf(text, sizeof(text), "on the ground");
        } else if (aircraft->altitude_valid) {
            char grouped[16];
            format_grouped(grouped, sizeof(grouped), aircraft->altitude_ft);
            (void)snprintf(text, sizeof(text), "%s ft", grouped);
        } else {
            (void)snprintf(text, sizeof(text), "altitude --");
        }
        set_label_if_changed(s_ui.trk_row_text[1], text);

        if (aircraft->vertical_rate_valid && !aircraft->ground) {
            const long rate = aircraft->vertical_rate_fpm;
            char grouped[16];
            format_grouped(grouped, sizeof(grouped), rate < 0 ? -rate : rate);
            if (rate == 0) {
                (void)snprintf(text, sizeof(text), "level");
            } else {
                (void)snprintf(text, sizeof(text), "%s %s fpm",
                               rate > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                               grouped);
            }
            set_label_if_changed(s_ui.trk_row_icon[2],
                                 rate < 0 ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
        } else {
            (void)snprintf(text, sizeof(text), "vertical rate --");
            set_label_if_changed(s_ui.trk_row_icon[2], LV_SYMBOL_UP);
        }
        set_label_if_changed(s_ui.trk_row_text[2], text);

        if (aircraft->ground_speed_valid && eta_s >= 0) {
            if (eta_s >= 3600) {
                (void)snprintf(text, sizeof(text), "%.0f kt " LV_SYMBOL_BULLET " ETA %ld:%02ld",
                               (double)aircraft->ground_speed_kt, eta_s / 3600,
                               (eta_s % 3600) / 60);
            } else {
                (void)snprintf(text, sizeof(text), "%.0f kt " LV_SYMBOL_BULLET " ETA %ldm",
                               (double)aircraft->ground_speed_kt, eta_s / 60);
            }
        } else if (aircraft->ground_speed_valid) {
            (void)snprintf(text, sizeof(text), "%.0f kt", (double)aircraft->ground_speed_kt);
        } else {
            (void)snprintf(text, sizeof(text), "speed --");
        }
        set_label_if_changed(s_ui.trk_row_text[3], text);
        for (size_t index = 0U; index < 4U; ++index) {
            lv_obj_set_style_text_color(s_ui.trk_row_text[index],
                                        lv_color_hex(value_color), 0);
            if (index == 2U) {
                lv_obj_set_style_text_color(s_ui.trk_row_icon[index], accent_color, 0);
            } else {
                lv_obj_set_style_image_recolor(s_ui.trk_row_icon[index], accent_color, 0);
            }
        }
    } else {
        show_group(s_ui.trk_data, false);
        show_group(s_ui.trk_empty, true);
        const bool sweeping = snapshot->state == AIRTRACK_FEED_EMPTY ||
                              snapshot->state == AIRTRACK_FEED_SEARCHING;
        set_radar_animation(sweeping);
        char focus_headline[32];
        set_label_if_changed(s_ui.trk_empty_head,
            state->settings->focus_flight[0] != '\0' &&
                    (snapshot->state == AIRTRACK_FEED_EMPTY ||
                     snapshot->state == AIRTRACK_FEED_SEARCHING)
                ? empty_focus_headline(focus_headline, sizeof(focus_headline),
                                       state->settings->focus_flight)
                : empty_headline(state));
        lv_obj_set_style_text_color(s_ui.trk_empty_head, state_color, 0);
        for (size_t index = 0U; index < 4U; ++index) {
            lv_obj_set_style_border_color(s_ui.trk_radar_ring[index], state_color, 0);
        }
        lv_obj_set_style_arc_color(s_ui.trk_radar_sweep, state_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_ui.trk_radar_dot, state_color, 0);

        const float radius = (float)state->settings->radius_nm *
                             unit_scale(state->settings->distance_unit);
        (void)snprintf(text, sizeof(text), "%.0f", (double)radius);
        set_label_if_changed(s_ui.trk_empty_radius, text);
        set_label_if_changed(s_ui.trk_empty_unit,
                             unit_name(state->settings->distance_unit));
        layout_value_pair(s_ui.trk_empty_radius, &lv_font_montserrat_28,
                          s_ui.trk_empty_unit, &lv_font_montserrat_16, 206, 11);
        lv_obj_set_style_text_color(s_ui.trk_empty_radius, state_color, 0);
        lv_obj_set_style_text_color(s_ui.trk_empty_unit, state_color, 0);

        const char *hint;
        switch (snapshot->state) {
        case AIRTRACK_FEED_EMPTY:
            hint = state->settings->focus_flight[0] != '\0'
                       ? "not reported in range yet"
                       : "sky is clear right now";
            break;
        case AIRTRACK_FEED_TIME_SYNC:
            hint = "waiting for network time";
            break;
        case AIRTRACK_FEED_CONFIG_REQUIRED:
            hint = "open the address below";
            break;
        case AIRTRACK_FEED_OFFLINE:
            hint = state->wifi_connected
                       ? airtrack_feed_error_name(snapshot->error)
                       : "reconnecting to Wi-Fi";
            break;
        default:
            hint = "requesting nearby traffic";
            break;
        }
        if (snapshot->state == AIRTRACK_FEED_OFFLINE && state->wifi_connected &&
            snapshot->retry_after_s > 0U) {
            (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " retry in %lus",
                           airtrack_feed_error_name(snapshot->error),
                           (unsigned long)snapshot->retry_after_s);
            hint = text;
        }
        set_label_if_changed(s_ui.trk_empty_hint, hint);
    }

    /* Footer: SSID • IP and attribution. */
    if (state->wifi_connected) {
        (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " %s",
                       state->ssid, state->ip_address);
    } else {
        (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " offline",
                       state->ssid);
    }
    set_label_if_changed(s_ui.trk_footer_net, text);
    lv_obj_set_style_text_color(s_ui.trk_footer_net,
        lv_color_hex(state->wifi_connected ? UI_COLOR_DIM : UI_COLOR_RED), 0);

    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    return ESP_OK;
}

esp_err_t ui_diagnostic_deinit(void)
{
    if (!s_ui.initialized && !s_ui.port_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ui.initialized = false;
    esp_err_t err = ESP_OK;
    if (s_ui.display != NULL) {
        if (!lvgl_port_lock(1000)) {
            return ESP_ERR_TIMEOUT;
        }
        lv_display_delete(s_ui.display);
        s_ui.display = NULL;
        s_ui.screen = NULL;
        if (s_ui.qr_draw_buffer != NULL) {
            lv_draw_buf_destroy(s_ui.qr_draw_buffer);
            s_ui.qr_draw_buffer = NULL;
        }
        lvgl_port_unlock();
    }

    s_flush_waiter = NULL;
    if (s_ui.callback_registered) {
        err = board_lcd_register_color_done_callback(NULL, NULL);
    }
    heap_caps_free(s_ui.draw_buffer_a);
    heap_caps_free(s_ui.draw_buffer_b);

    const esp_err_t deinit_err =
        s_ui.port_started ? lvgl_port_deinit() : ESP_OK;
    s_ui = (ui_context_t) {0};
    return err != ESP_OK ? err : deinit_err;
}

bool ui_diagnostic_is_initialized(void)
{
    return s_ui.initialized;
}
