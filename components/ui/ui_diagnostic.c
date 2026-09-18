#include "ui_diagnostic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#define UI_FOCUS_ROWS 5U

/*
 * The screens are laid out against a 172-pixel design width, which is the
 * narrowest supported panel.  Full-width elements (headers, dividers, footer,
 * centred labels) already derive from BOARD_LCD_H_RES.  The few blocks that
 * are positioned absolutely - the distance/unit pair, the compass, the radar,
 * and the route columns flanking it - are shifted by UI_WIDE_PAD so they stay
 * centred on a wider panel.  The constant is exactly zero at the design
 * width, so the 172-pixel rendering is unchanged.
 */
#define UI_DESIGN_WIDTH 172
#define UI_WIDE_PAD (((int32_t)BOARD_LCD_H_RES - UI_DESIGN_WIDTH) / 2)

/*
 * The diagnostic screen is a stack of full-width rows rather than a composed
 * layout, so it simply grows with the panel: content is inset 12 px a side,
 * footer text 10 px.  Both evaluate to the original 148 and 152 at the design
 * width.
 */
#if BOARD_HAS_BATTERY_SENSE
/*
 * Battery gauge, drawn top right the way a phone does it: an outlined body
 * with a nub, filled in proportion, and the figure beside it.  Sized to sit
 * inside the 24-pixel header bar.
 */
#define UI_BATT_BODY_W 22
#define UI_BATT_BODY_H 12
#define UI_BATT_BODY_X ((int32_t)BOARD_LCD_H_RES - 30)
#define UI_BATT_BODY_Y 6
#define UI_BATT_NUB_W 3
#define UI_BATT_NUB_H 6
/* Inside the 1-pixel border, with a pixel of air on each side. */
#define UI_BATT_FILL_X (UI_BATT_BODY_X + 2)
#define UI_BATT_FILL_Y (UI_BATT_BODY_Y + 2)
#define UI_BATT_FILL_MAX (UI_BATT_BODY_W - 4)
#define UI_BATT_FILL_H (UI_BATT_BODY_H - 4)
/* Figure sits to the left of the body, right-aligned against it. */
#define UI_BATT_TEXT_W 44
#define UI_BATT_TEXT_X (UI_BATT_BODY_X - UI_BATT_TEXT_W - 4)
/* The updated-age value moves left to clear the gauge. */
#define UI_HDR_RIGHT_W 40
#define UI_HDR_RIGHT_X (UI_BATT_TEXT_X - UI_HDR_RIGHT_W - 4)
#endif

#define UI_CONTENT_X 12
#define UI_CONTENT_WIDTH ((int32_t)BOARD_LCD_H_RES - (2 * UI_CONTENT_X))
#define UI_FOOTER_X 10
#define UI_FOOTER_WIDTH ((int32_t)BOARD_LCD_H_RES - (2 * UI_FOOTER_X))

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
#if BOARD_HAS_BATTERY_SENSE
    lv_obj_t *hdr_batt_body;
    lv_obj_t *hdr_batt_nub;
    lv_obj_t *hdr_batt_fill;
    lv_obj_t *hdr_batt_text;
#endif
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
    /* Followed-flight view, shown instead of both blocks above. */
    lv_obj_t *trk_focus;
    lv_obj_t *fcs_logo;
    lv_obj_t *fcs_ident;
    lv_obj_t *fcs_meta;
    lv_obj_t *fcs_from;
    lv_obj_t *fcs_to;
    lv_obj_t *fcs_mid;
    lv_obj_t *fcs_track;
    lv_obj_t *fcs_fill;
    lv_obj_t *fcs_plane;
    lv_obj_t *fcs_dep;
    lv_obj_t *fcs_arr;
    lv_obj_t *fcs_phase;
    lv_obj_t *fcs_divider;
    lv_obj_t *fcs_row_icon[UI_FOCUS_ROWS];
    lv_obj_t *fcs_row_text[UI_FOCUS_ROWS];
    uint32_t logo_generation;
    bool logo_shown;
    /* Update screen widgets. */
    bool updating_visible;
    lv_obj_t *upd_version;
    lv_obj_t *upd_fill;
    lv_obj_t *upd_percent;
    lv_obj_t *upd_phase;
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
    /* The name column is fixed; the value takes whatever the panel leaves. */
    const int32_t row_width = UI_CONTENT_WIDTH;
    const int32_t value_x = 69;
    const int32_t value_width = row_width - value_x - 8;

    lv_obj_t *line = lv_obj_create(screen);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, row_width, 34);
    lv_obj_set_pos(line, UI_CONTENT_X, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x121E31), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 5, 0);

    create_label(line, name, 8, 9, 58, lv_color_hex(0xA7B5CA));
    *value = create_label(line, "CHECKING", value_x, 9, value_width,
                          result_color(UI_DIAGNOSTIC_PENDING));
    /*
     * Hold the value to one line.  create_label() leaves the height on
     * content, and LV_LABEL_LONG_DOT only ellipsizes once the text runs out of
     * height, so an unbounded label wraps a long value such as
     * "16MB OPTIONAL" onto a second line that the row then clips.
     */
    lv_obj_set_height(*value, lv_font_get_line_height(&lv_font_montserrat_14));
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
#if BOARD_HAS_BATTERY_SENSE
    s_ui.hdr_right = create_font_label(bar, right != NULL ? right : "",
                                       UI_HDR_RIGHT_X, 6, UI_HDR_RIGHT_W,
                                       &lv_font_montserrat_12,
                                       UI_COLOR_GREEN);
#else
    s_ui.hdr_right = create_font_label(bar, right != NULL ? right : "", 128,
                                       6, 38, &lv_font_montserrat_12,
                                       UI_COLOR_GREEN);
#endif
    lv_obj_set_style_text_align(s_ui.hdr_right, LV_TEXT_ALIGN_RIGHT, 0);

#if BOARD_HAS_BATTERY_SENSE
    /* Hidden until a reading arrives, so a board with no cell shows nothing. */
    s_ui.hdr_batt_body = create_group(bar, UI_BATT_BODY_X, UI_BATT_BODY_Y,
                                      UI_BATT_BODY_W, UI_BATT_BODY_H);
    lv_obj_set_style_radius(s_ui.hdr_batt_body, 3, 0);
    lv_obj_set_style_border_width(s_ui.hdr_batt_body, 1, 0);
    lv_obj_set_style_border_color(s_ui.hdr_batt_body,
                                  lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_border_opa(s_ui.hdr_batt_body, LV_OPA_COVER, 0);

    s_ui.hdr_batt_nub = create_panel(
        bar, UI_BATT_BODY_X + UI_BATT_BODY_W, UI_BATT_BODY_Y +
        ((UI_BATT_BODY_H - UI_BATT_NUB_H) / 2), UI_BATT_NUB_W, UI_BATT_NUB_H,
        UI_COLOR_DIM);
    lv_obj_set_style_radius(s_ui.hdr_batt_nub, 1, 0);

    s_ui.hdr_batt_fill = create_panel(bar, UI_BATT_FILL_X, UI_BATT_FILL_Y,
                                      UI_BATT_FILL_MAX, UI_BATT_FILL_H,
                                      UI_COLOR_GREEN);
    lv_obj_set_style_radius(s_ui.hdr_batt_fill, 1, 0);

    s_ui.hdr_batt_text = create_font_label(bar, "", UI_BATT_TEXT_X, 6,
                                           UI_BATT_TEXT_W,
                                           &lv_font_montserrat_12,
                                           UI_COLOR_DIM);
    lv_obj_set_style_text_align(s_ui.hdr_batt_text, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_add_flag(s_ui.hdr_batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.hdr_batt_nub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.hdr_batt_fill, LV_OBJ_FLAG_HIDDEN);
#endif

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
    lv_obj_t *mode = create_label(header, "HW", 97, 8,
                                  (int32_t)BOARD_LCD_H_RES - 10 - 97,
                                  lv_color_hex(0x8BE36D));
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_RIGHT, 0);

    create_label(s_ui.screen, "SYSTEM BRING-UP", UI_CONTENT_X, 43,
                 UI_CONTENT_WIDTH, lv_color_hex(0x6F819B));
    s_ui.phase = create_label(s_ui.screen, "Starting hardware...",
                              UI_CONTENT_X, 64, UI_CONTENT_WIDTH,
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
    create_label(footer, "NETWORK", UI_FOOTER_X, 9, UI_FOOTER_WIDTH,
                 lv_color_hex(0x6F819B));
    s_ui.ssid = create_label(footer, "SSID: --", UI_FOOTER_X, 31,
                             UI_FOOTER_WIDTH, lv_color_hex(0xDDE7F4));
    s_ui.ip = create_label(footer, "IP: --", UI_FOOTER_X, 51, UI_FOOTER_WIDTH,
                           lv_color_hex(0xDDE7F4));
    create_label(footer, "Data: adsb.fi", UI_FOOTER_X, 72, UI_FOOTER_WIDTH,
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
    s_ui.updating_visible = false;
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
#define UI_COMPASS_CX (86 + UI_WIDE_PAD)
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
    s_ui.trk_from_caption = create_font_label(parent, "", 4 + UI_WIDE_PAD,
                                              top, 40,
                                              &lv_font_montserrat_10,
                                              UI_COLOR_MUTED);
    lv_obj_set_style_text_align(s_ui.trk_from_caption, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_from = create_font_label(parent, "", 4 + UI_WIDE_PAD, top + 12,
                                      40, &lv_font_montserrat_14,
                                      UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.trk_from, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_ui.trk_from, -1, 0);
    s_ui.trk_to_caption = create_font_label(parent, "",
                                            BOARD_LCD_H_RES - 44 - UI_WIDE_PAD,
                                            top, 40, &lv_font_montserrat_10,
                                            UI_COLOR_MUTED);
    lv_obj_set_style_text_align(s_ui.trk_to_caption, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_to = create_font_label(parent, "",
                                    BOARD_LCD_H_RES - 44 - UI_WIDE_PAD,
                                    top + 12, 40, &lv_font_montserrat_14,
                                    UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.trk_to, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_ui.trk_to, -1, 0);
}

#if BOARD_HAS_BATTERY_SENSE
/* Defined below, with the other tracking-screen update helpers. */
static void set_label_if_changed(lv_obj_t *label, const char *text);

/*
 * Redraw the gauge.  While USB is attached the charger holds the sense rail
 * near full whatever the cell is doing, so the bar is shown full with a
 * charging bolt rather than a percentage that would describe the charger.
 */
static void update_battery_gauge(bool valid, bool usb_present, uint8_t percent)
{
    if (s_ui.hdr_batt_body == NULL) {
        return;
    }
    if (!valid) {
        lv_obj_add_flag(s_ui.hdr_batt_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.hdr_batt_nub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.hdr_batt_fill, LV_OBJ_FLAG_HIDDEN);
        set_label_if_changed(s_ui.hdr_batt_text, "");
        return;
    }
    lv_obj_remove_flag(s_ui.hdr_batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.hdr_batt_nub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.hdr_batt_fill, LV_OBJ_FLAG_HIDDEN);

    uint32_t colour;
    int32_t width;
    char text[8];
    if (usb_present) {
        colour = UI_COLOR_CYAN;
        width = UI_BATT_FILL_MAX;
        (void)snprintf(text, sizeof(text), "%s", LV_SYMBOL_CHARGE);
    } else {
        colour = percent > 50U ? UI_COLOR_GREEN
               : percent > 20U ? UI_COLOR_AMBER
                               : UI_COLOR_RED;
        width = ((int32_t)percent * UI_BATT_FILL_MAX + 50) / 100;
        /* Keep a sliver visible so an almost-flat cell still reads as a bar. */
        if (width < 1 && percent > 0U) {
            width = 1;
        }
        (void)snprintf(text, sizeof(text), "%u%%", (unsigned)percent);
    }
    lv_obj_set_width(s_ui.hdr_batt_fill, width);
    lv_obj_set_style_bg_color(s_ui.hdr_batt_fill, lv_color_hex(colour), 0);
    lv_obj_set_style_border_color(s_ui.hdr_batt_body, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_color(s_ui.hdr_batt_nub, lv_color_hex(colour), 0);
    set_label_if_changed(s_ui.hdr_batt_text, text);
    lv_obj_set_style_text_color(s_ui.hdr_batt_text, lv_color_hex(colour), 0);
}
#endif

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

/*
 * Followed-flight layout (y relative to the content group under the header):
 *
 *     [logo] CALLSIGN          4   airline or type, registration   40
 *     MSP   1,204 mi to go   SEA    58   codes and distance to go
 *     ===========>-----------        88   route progress with the plane
 *     DEP 14:05         ARR 17:22  102   times, amber when late
 *            CRUISING              122   phase
 *     rows: altitude, speed/ETA, position, gates or airframe, age  152..
 */
#define UI_FOCUS_LOGO_Y 4
#define UI_FOCUS_META_Y 40
#define UI_FOCUS_ROUTE_Y 58
#define UI_FOCUS_BAR_Y 88
#define UI_FOCUS_BAR_X 14
#define UI_FOCUS_BAR_W ((int32_t)BOARD_LCD_H_RES - (2 * UI_FOCUS_BAR_X))
#define UI_FOCUS_TIMES_Y 102
#define UI_FOCUS_PHASE_Y 122
#define UI_FOCUS_DIVIDER_Y 146
#define UI_FOCUS_ROW_Y 152
#define UI_FOCUS_ROW_STEP 24
/* Panels wider than the 172-pixel design have room for fuller wording. */
#define UI_FOCUS_WIDE (BOARD_LCD_H_RES >= 200)

static uint16_t s_logo_pixels[FLIGHT_INFO_LOGO_SIZE * FLIGHT_INFO_LOGO_SIZE];
static lv_image_dsc_t s_logo_image = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = FLIGHT_INFO_LOGO_SIZE,
        .h = FLIGHT_INFO_LOGO_SIZE,
        .stride = FLIGHT_INFO_LOGO_SIZE * 2U,
    },
    .data_size = sizeof(s_logo_pixels),
    .data = (const uint8_t *)s_logo_pixels,
};

/* A label only ever shows one line, ending in "..." when it runs out. */
static void one_line(lv_obj_t *label, const lv_font_t *font)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

static void create_focus_row(lv_obj_t *parent, size_t index,
                             const lv_image_dsc_t *icon, const char *symbol)
{
    const int32_t y = UI_FOCUS_ROW_Y + ((int32_t)index * UI_FOCUS_ROW_STEP);
    if (index > 0U) {
        create_hline(parent, 14, y - 4, BOARD_LCD_H_RES - 28, 0x1C2A3D);
    }
    if (icon != NULL) {
        lv_obj_t *image = lv_image_create(parent);
        lv_image_set_src(image, icon);
        lv_obj_set_pos(image, 18, y + 1);
        lv_obj_set_style_image_recolor(image, lv_color_hex(UI_COLOR_CYAN), 0);
        lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
        s_ui.fcs_row_icon[index] = image;
    } else {
        s_ui.fcs_row_icon[index] = create_font_label(
            parent, symbol, 18, y + 1, 18, &lv_font_montserrat_14, UI_COLOR_CYAN);
    }
    s_ui.fcs_row_text[index] = create_font_label(
        parent, "", 42, y, BOARD_LCD_H_RES - 48, &lv_font_montserrat_14,
        UI_COLOR_TEXT);
    one_line(s_ui.fcs_row_text[index], &lv_font_montserrat_14);
}

static void create_focus_group(lv_obj_t *screen)
{
    lv_obj_t *group = create_group(screen, 0, 24, BOARD_LCD_H_RES, 272);
    s_ui.trk_focus = group;

    s_ui.fcs_logo = lv_image_create(group);
    lv_image_set_src(s_ui.fcs_logo, &s_logo_image);
    lv_obj_set_pos(s_ui.fcs_logo, 0, UI_FOCUS_LOGO_Y);
    lv_obj_add_flag(s_ui.fcs_logo, LV_OBJ_FLAG_HIDDEN);
    s_ui.fcs_ident = create_font_label(group, "", 0, UI_FOCUS_LOGO_Y + 1,
                                       BOARD_LCD_H_RES, &lv_font_montserrat_28,
                                       UI_COLOR_TEXT);
    s_ui.fcs_meta = create_centered_label(group, "", UI_FOCUS_META_Y,
                                          &lv_font_montserrat_12, UI_COLOR_DIM);
    one_line(s_ui.fcs_meta, &lv_font_montserrat_12);

    s_ui.fcs_from = create_font_label(group, "", 10, UI_FOCUS_ROUTE_Y, 60,
                                      &lv_font_montserrat_20, UI_COLOR_TEXT);
    s_ui.fcs_to = create_font_label(group, "", BOARD_LCD_H_RES - 70,
                                    UI_FOCUS_ROUTE_Y, 60, &lv_font_montserrat_20,
                                    UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.fcs_to, LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.fcs_mid = create_centered_label(group, "", UI_FOCUS_ROUTE_Y + 7,
                                         &lv_font_montserrat_12, UI_COLOR_DIM);
    /* Keep the distance between the two codes rather than over them. */
    lv_obj_set_width(s_ui.fcs_mid, BOARD_LCD_H_RES - 124);
    lv_obj_set_x(s_ui.fcs_mid, 62);
    one_line(s_ui.fcs_mid, &lv_font_montserrat_12);

    s_ui.fcs_track = create_panel(group, UI_FOCUS_BAR_X, UI_FOCUS_BAR_Y,
                                  UI_FOCUS_BAR_W, 6, 0x1C2A3D);
    lv_obj_set_style_radius(s_ui.fcs_track, 3, 0);
    s_ui.fcs_fill = create_panel(s_ui.fcs_track, 0, 0, 1, 6, UI_COLOR_CYAN);
    lv_obj_set_style_radius(s_ui.fcs_fill, 3, 0);
    s_ui.fcs_plane = lv_image_create(group);
    lv_image_set_src(s_ui.fcs_plane, &ui_icon_plane);
    lv_image_set_pivot(s_ui.fcs_plane, 14, 14);
    lv_image_set_scale(s_ui.fcs_plane, 170); /* ~19 px */
    lv_image_set_rotation(s_ui.fcs_plane, 900); /* nose to the destination */
    lv_obj_set_style_image_recolor(s_ui.fcs_plane, lv_color_hex(UI_COLOR_CYAN), 0);
    lv_obj_set_style_image_recolor_opa(s_ui.fcs_plane, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_ui.fcs_plane, UI_FOCUS_BAR_X - 14, UI_FOCUS_BAR_Y + 3 - 14);

    s_ui.fcs_dep = create_font_label(group, "", 10, UI_FOCUS_TIMES_Y,
                                     BOARD_LCD_H_RES / 2 - 10,
                                     &lv_font_montserrat_12, UI_COLOR_DIM);
    s_ui.fcs_arr = create_font_label(group, "", BOARD_LCD_H_RES / 2,
                                     UI_FOCUS_TIMES_Y, BOARD_LCD_H_RES / 2 - 10,
                                     &lv_font_montserrat_12, UI_COLOR_DIM);
    lv_obj_set_style_text_align(s_ui.fcs_arr, LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.fcs_phase = create_centered_label(group, "", UI_FOCUS_PHASE_Y,
                                           &lv_font_montserrat_16, UI_COLOR_CYAN);
    one_line(s_ui.fcs_phase, &lv_font_montserrat_16);
    s_ui.fcs_divider = create_hline(group, 14, UI_FOCUS_DIVIDER_Y,
                                    BOARD_LCD_H_RES - 28, UI_COLOR_CYAN);

    create_focus_row(group, 0U, &ui_icon_mountain, NULL);
    create_focus_row(group, 1U, &ui_icon_gauge, NULL);
    create_focus_row(group, 2U, &ui_icon_nav, NULL);
    create_focus_row(group, 3U, NULL, LV_SYMBOL_HOME);
    create_focus_row(group, 4U, NULL, LV_SYMBOL_EYE_OPEN);
    lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    s_ui.logo_generation = 0U;
    s_ui.logo_shown = false;
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
    one_line(s_ui.trk_meta, &lv_font_montserrat_12);
    s_ui.trk_divider = create_hline(s_ui.trk_data, 14, 54,
                                    BOARD_LCD_H_RES - 28, UI_COLOR_CYAN);
    s_ui.trk_distance = create_font_label(s_ui.trk_data, "--",
                                          UI_WIDE_PAD, 55, 120,
                                          &lv_font_montserrat_40,
                                          UI_COLOR_CYAN);
    s_ui.trk_unit = create_font_label(s_ui.trk_data, "NM",
                                      120 + UI_WIDE_PAD, 73, 46,
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
    create_radar(s_ui.trk_empty, 86 + UI_WIDE_PAD, 126);
    s_ui.trk_empty_within = create_centered_label(s_ui.trk_empty, "within",
                                                  188, &lv_font_montserrat_14,
                                                  UI_COLOR_TEXT);
    s_ui.trk_empty_radius = create_font_label(s_ui.trk_empty, "25",
                                              UI_WIDE_PAD, 206,
                                              110, &lv_font_montserrat_28,
                                              UI_COLOR_CYAN);
    s_ui.trk_empty_unit = create_font_label(s_ui.trk_empty, "NM",
                                            110 + UI_WIDE_PAD, 218,
                                            50, &lv_font_montserrat_16,
                                            UI_COLOR_CYAN);
    s_ui.trk_empty_hint = create_centered_label(s_ui.trk_empty, "", 246,
                                                &lv_font_montserrat_12,
                                                UI_COLOR_MUTED);
    lv_obj_add_flag(s_ui.trk_empty, LV_OBJ_FLAG_HIDDEN);

    create_focus_group(screen);

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
    s_ui.updating_visible = false;
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

static void format_clock(char *out, size_t capacity, int64_t epoch)
{
    const time_t seconds = (time_t)epoch;
    struct tm local;
    if (epoch <= 0 || localtime_r(&seconds, &local) == NULL) {
        (void)snprintf(out, capacity, "--:--");
        return;
    }
    (void)snprintf(out, capacity, "%02d:%02d", local.tm_hour, local.tm_min);
}

static void format_duration(char *out, size_t capacity, long seconds)
{
    if (seconds >= 3600) {
        (void)snprintf(out, capacity, "%ld:%02ld", seconds / 3600, (seconds % 3600) / 60);
    } else {
        (void)snprintf(out, capacity, "%ldm", seconds / 60);
    }
}

static void format_age(char *out, size_t capacity, double seconds)
{
    if (seconds < 90.0) {
        (void)snprintf(out, capacity, "%.0fs", seconds);
    } else if (seconds < 5400.0) {
        (void)snprintf(out, capacity, "%.0fm", seconds / 60.0);
    } else {
        (void)snprintf(out, capacity, "%.1fh", seconds / 3600.0);
    }
}

static const char *focus_phase_text(airtrack_flight_phase_t phase,
                                    const airtrack_aircraft_t *aircraft,
                                    const flight_schedule_t *schedule,
                                    uint32_t *color)
{
    *color = UI_COLOR_CYAN;
    switch (phase) {
    case AIRTRACK_PHASE_GROUND:
        return aircraft != NULL && aircraft->ground_speed_valid &&
                       aircraft->ground_speed_kt > 5.0f
                   ? "TAXIING" : "AT THE GATE";
    case AIRTRACK_PHASE_CLIMB:
        return "CLIMBING";
    case AIRTRACK_PHASE_CRUISE:
        return "CRUISING";
    case AIRTRACK_PHASE_DESCENT:
        return "DESCENDING";
    case AIRTRACK_PHASE_APPROACH:
        return "ON APPROACH";
    case AIRTRACK_PHASE_LANDED:
        *color = UI_COLOR_GREEN;
        return "LANDED";
    case AIRTRACK_PHASE_LOST:
        *color = UI_COLOR_AMBER;
        return "SIGNAL LOST";
    default:
        break;
    }
    *color = UI_COLOR_DIM;
    if (schedule != NULL && strstr(schedule->status, "cancel") != NULL) {
        *color = UI_COLOR_RED;
        return "CANCELLED";
    }
    return aircraft != NULL ? "TRACKING" : schedule != NULL ? "SCHEDULED" : "NOT AIRBORNE";
}

static void update_focus_view(const ui_tracking_state_t *state, double since_success)
{
    const airtrack_snapshot_t *snapshot = state->snapshot;
    const airtrack_settings_t *settings = state->settings;
    const flight_info_t *flight =
        state->flight != NULL && strcmp(state->flight->code, settings->focus_flight) == 0
            ? state->flight : NULL;
    const airtrack_aircraft_t *aircraft =
        snapshot->aircraft_count > 0U &&
                airtrack_aircraft_matches(&snapshot->aircraft[0], settings->focus_flight)
            ? &snapshot->aircraft[0] : NULL;
    const flight_route_t *route = flight != NULL && flight->route.valid ? &flight->route : NULL;
    const flight_schedule_t *schedule =
        flight != NULL && flight->schedule_state == FLIGHT_SCHEDULE_OK ? &flight->schedule : NULL;
    const float scale = unit_scale(settings->distance_unit);
    const char *unit = unit_name(settings->distance_unit);
    const double age = aircraft != NULL
        ? (double)aircraft->seen_pos_s + (since_success > 0.0 ? since_success : 0.0) : -1.0;
    const bool live = aircraft != NULL && age <= UI_STALE_AGE_S &&
                      snapshot->state == AIRTRACK_FEED_LIVE;
    const uint32_t value_color = live ? UI_COLOR_TEXT : UI_COLOR_DIM;
    const bool emergency = aircraft != NULL && aircraft->emergency;
    char text[96];
    char clock[8];

    /* Logo and callsign, centred together. */
    const bool logo = state->logo != NULL && flight != NULL && flight->logo_valid;
    if (logo && state->logo_generation != s_ui.logo_generation) {
        memcpy(s_logo_pixels, state->logo, sizeof(s_logo_pixels));
        s_ui.logo_generation = state->logo_generation;
        /* LVGL's image cache is disabled (LV_CACHE_DEF_SIZE 0), so the new
         * pixels are read on the next draw. */
        lv_obj_invalidate(s_ui.fcs_logo);
    }
    if (logo != s_ui.logo_shown) {
        show_group(s_ui.fcs_logo, logo);
        s_ui.logo_shown = logo;
    }
    const char *ident = route != NULL && route->callsign_icao[0] != '\0' ? route->callsign_icao
                        : aircraft != NULL && aircraft->callsign[0] != '\0' ? aircraft->callsign
                                                                            : settings->focus_flight;
    set_label_if_changed(s_ui.fcs_ident, ident);
    lv_point_t size;
    lv_text_get_size(&size, ident, &lv_font_montserrat_28, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int32_t logo_width = logo ? (int32_t)FLIGHT_INFO_LOGO_SIZE + 8 : 0;
    int32_t start = ((int32_t)BOARD_LCD_H_RES - (logo_width + size.x)) / 2;
    start = start < 4 ? 4 : start;
    lv_obj_set_x(s_ui.fcs_logo, start);
    lv_obj_set_x(s_ui.fcs_ident, start + logo_width);
    lv_obj_set_width(s_ui.fcs_ident,
                     LV_MIN(size.x + 2, (int32_t)BOARD_LCD_H_RES - start - logo_width - 4));
    lv_obj_set_style_text_color(s_ui.fcs_ident,
        lv_color_hex(emergency ? UI_COLOR_RED : live ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);

    /* Airline, then airframe: ADS-B's name for it, Flystack's model, or
     * at worst the ICAO designator. */
    const char *type = aircraft != NULL && aircraft->aircraft_type[0] != '\0'
                           ? aircraft->aircraft_type
                       : schedule != NULL ? schedule->aircraft_icao : "";
    const char *airframe = aircraft != NULL && aircraft->description[0] != '\0'
                               ? aircraft->description
                           : schedule != NULL && schedule->model[0] != '\0' ? schedule->model
                                                                           : type;
    const char *registration = aircraft != NULL && aircraft->registration[0] != '\0'
                                   ? aircraft->registration
                               : schedule != NULL ? schedule->registration : "";
    const bool gates = schedule != NULL &&
                       (schedule->dep_gate[0] != '\0' || schedule->arr_gate[0] != '\0');
    if (!UI_FOCUS_WIDE && gates && airframe[0] != '\0') {
        /* Narrow, with the gates taking the airframe's row: the logo says
         * whose flight it is, so the airframe goes up here. */
        (void)snprintf(text, sizeof(text), "%s", airframe);
    } else if (route != NULL && route->airline_name[0] != '\0') {
        /* A narrow panel shows the airframe on its own row instead. */
        const bool both = UI_FOCUS_WIDE && airframe[0] != '\0';
        (void)snprintf(text, sizeof(text), "%s%s%s", route->airline_name,
                       both ? " " LV_SYMBOL_BULLET " " : "", both ? airframe : "");
    } else {
        (void)snprintf(text, sizeof(text), "%s%s%s", airframe,
                       airframe[0] != '\0' && registration[0] != '\0' ? " " LV_SYMBOL_BULLET " " : "",
                       registration);
    }
    set_label_if_changed(s_ui.fcs_meta, text);

    /* Route codes, distance to go, and progress. */
    /* Route order: as ADS-B has seen the aircraft fly it; before that,
     * Flystack's leg for today; before that, adsbdb's usual order. */
    const bool confirmed = aircraft != NULL && aircraft->route_valid &&
                           aircraft->route_confirmed;
    const bool timetable = schedule != NULL && schedule->dep_iata[0] != '\0' &&
                           schedule->arr_iata[0] != '\0';
    const char *from = confirmed ? aircraft->route_from
                       : timetable ? schedule->dep_iata
                       : route != NULL && route->origin[0] != '\0' ? route->origin
                       : aircraft != NULL && aircraft->route_valid ? aircraft->route_from
                                                                   : "---";
    const char *to = confirmed ? aircraft->route_to
                     : timetable ? schedule->arr_iata
                     : route != NULL && route->destination[0] != '\0' ? route->destination
                     : aircraft != NULL && aircraft->route_valid ? aircraft->route_to
                                                                 : "---";
    set_label_if_changed(s_ui.fcs_from, from);
    set_label_if_changed(s_ui.fcs_to, to);
    lv_obj_set_style_text_color(s_ui.fcs_from, lv_color_hex(value_color), 0);
    lv_obj_set_style_text_color(s_ui.fcs_to, lv_color_hex(value_color), 0);

    float remaining_nm = -1.0f;
    long eta_s = -1;
    if (aircraft != NULL && aircraft->destination_valid && aircraft->route_confirmed) {
        float bearing = 0.0f;
        airtrack_geometry(aircraft->latitude, aircraft->longitude,
                          aircraft->destination_latitude,
                          aircraft->destination_longitude, &remaining_nm, &bearing);
        if (aircraft->ground_speed_valid && aircraft->ground_speed_kt >= 60.0f) {
            eta_s = (long)(remaining_nm / aircraft->ground_speed_kt * 3600.0f);
        }
    }
    const airtrack_flight_phase_t phase = flight != NULL ? flight->phase
        : aircraft != NULL ? airtrack_flight_phase(aircraft, (float)age, false, remaining_nm)
                           : AIRTRACK_PHASE_UNKNOWN;
    const bool landed = phase == AIRTRACK_PHASE_LANDED;
    const bool was_airborne = flight != NULL ? flight->was_airborne : aircraft != NULL;
    if (remaining_nm >= 1.0f && !landed) {
        char grouped[16];
        format_grouped(grouped, sizeof(grouped), lroundf(remaining_nm * scale));
        (void)snprintf(text, sizeof(text), "%s %s%s", grouped, unit,
                       UI_FOCUS_WIDE ? " to go" : "");
    } else {
        text[0] = '\0';
    }
    set_label_if_changed(s_ui.fcs_mid, text);

    const int64_t now = (int64_t)time(NULL);
    const int64_t dep_estimate = schedule != NULL && schedule->dep_time > 0
        ? schedule->dep_time + 60LL * (schedule->dep_delay_min != FLIGHT_DELAY_UNKNOWN
                                           ? schedule->dep_delay_min : 0) : 0;
    const int64_t arr_estimate = schedule != NULL && schedule->arr_time > 0
        ? schedule->arr_time + 60LL * (schedule->arr_delay_min != FLIGHT_DELAY_UNKNOWN
                                           ? schedule->arr_delay_min : 0) : 0;
    float progress = aircraft != NULL ? airtrack_route_progress(aircraft) : -1.0f;
    if (landed) {
        progress = 1.0f;
    } else if (progress < 0.0f && !was_airborne && aircraft == NULL) {
        /* Not reporting yet: still at the origin.  On the ground with the
         * direction unconfirmed it could be either end, so no marker. */
        progress = 0.0f;
    } else if (progress < 0.0f && dep_estimate > 0 && arr_estimate > dep_estimate) {
        progress = (float)(now - dep_estimate) / (float)(arr_estimate - dep_estimate);
    }
    const bool progress_known = progress >= 0.0f;
    progress = progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress;
    const int32_t filled = (int32_t)lroundf(progress * (float)UI_FOCUS_BAR_W);
    lv_obj_set_width(s_ui.fcs_fill, filled > 0 ? filled : 1);
    /* The marker's centre stays inside the bar so it never leaves the panel. */
    const int32_t marker = LV_CLAMP(UI_FOCUS_BAR_X + 8, UI_FOCUS_BAR_X + filled,
                                    UI_FOCUS_BAR_X + UI_FOCUS_BAR_W - 8);
    lv_obj_set_x(s_ui.fcs_plane, marker - 14);
    show_group(s_ui.fcs_plane, progress_known);
    /* Landed reads as done, a flight not yet seen as waiting, and a held
     * position as stale. */
    const uint32_t accent = emergency ? UI_COLOR_RED
                            : landed ? UI_COLOR_GREEN
                            : live || aircraft == NULL ? UI_COLOR_CYAN : UI_COLOR_AMBER;
    lv_obj_set_style_bg_color(s_ui.fcs_fill, lv_color_hex(accent), 0);
    lv_obj_set_style_image_recolor(s_ui.fcs_plane, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_color(s_ui.fcs_divider, lv_color_hex(accent), 0);

    /* Departure and arrival times, local to this display.  What ADS-B saw
     * (takeoff, touchdown, the ground-speed estimate) beats the timetable. */
    const int64_t departure = flight != NULL && flight->takeoff_epoch > 0
                                  ? flight->takeoff_epoch : dep_estimate;
    if (departure > 0) {
        format_clock(clock, sizeof(clock), departure);
        (void)snprintf(text, sizeof(text), "DEP %s", clock);
    } else {
        text[0] = '\0';
    }
    set_label_if_changed(s_ui.fcs_dep, text);
    lv_obj_set_style_text_color(s_ui.fcs_dep,
        lv_color_hex(schedule != NULL && schedule->dep_time > 0 &&
                             departure - schedule->dep_time > 15 * 60
                         ? UI_COLOR_AMBER : UI_COLOR_DIM), 0);
    int64_t arrival = 0;
    if (landed) {
        arrival = flight != NULL ? flight->landing_epoch : 0;
    } else if (was_airborne && flight != NULL && flight->eta_epoch > 0) {
        arrival = flight->eta_epoch;
    } else if (was_airborne && eta_s >= 0) {
        arrival = now + eta_s; /* live estimate from ground speed */
    } else if (arr_estimate > 0) {
        arrival = arr_estimate;
    }
    if (arrival > 0) {
        format_clock(clock, sizeof(clock), arrival);
        (void)snprintf(text, sizeof(text), "%s %s", landed ? "LANDED" : "ARR", clock);
    } else {
        text[0] = '\0';
    }
    set_label_if_changed(s_ui.fcs_arr, text);
    lv_obj_set_style_text_color(s_ui.fcs_arr,
        lv_color_hex(schedule != NULL && schedule->arr_time > 0 &&
                             arrival - schedule->arr_time > 15 * 60
                         ? UI_COLOR_AMBER : UI_COLOR_DIM), 0);

    /* Phase. */
    uint32_t phase_color = UI_COLOR_CYAN;
    const char *phase_text = focus_phase_text(phase, aircraft, schedule, &phase_color);
    if (emergency) {
        (void)snprintf(text, sizeof(text), "EMERGENCY %s", aircraft->squawk);
        phase_text = text;
        phase_color = UI_COLOR_RED;
    } else if (!live && aircraft != NULL && phase != AIRTRACK_PHASE_LANDED &&
               phase != AIRTRACK_PHASE_LOST && phase_color == UI_COLOR_CYAN) {
        phase_color = UI_COLOR_AMBER;
    }
    set_label_if_changed(s_ui.fcs_phase, phase_text);
    lv_obj_set_style_text_color(s_ui.fcs_phase, lv_color_hex(phase_color), 0);

    /* Rows: altitude, speed and ETA, position, gates or airframe, age. */
    if (aircraft == NULL) {
        (void)snprintf(text, sizeof(text), "--");
    } else if (aircraft->ground) {
        (void)snprintf(text, sizeof(text), "on the ground");
    } else if (aircraft->altitude_valid) {
        char grouped[16];
        format_grouped(grouped, sizeof(grouped), aircraft->altitude_ft);
        char rate[24] = "";
        if (aircraft->vertical_rate_valid && aircraft->vertical_rate_fpm != 0) {
            char rate_grouped[16];
            format_grouped(rate_grouped, sizeof(rate_grouped),
                           labs((long)aircraft->vertical_rate_fpm));
            (void)snprintf(rate, sizeof(rate), " %s %s",
                           aircraft->vertical_rate_fpm > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                           rate_grouped);
        }
        (void)snprintf(text, sizeof(text), "%s ft%s", grouped, rate);
    } else {
        (void)snprintf(text, sizeof(text), "altitude --");
    }
    set_label_if_changed(s_ui.fcs_row_text[0], text);

    if (aircraft != NULL && aircraft->ground_speed_valid) {
        if (eta_s >= 0 && !landed && !aircraft->ground) {
            char duration[16];
            format_duration(duration, sizeof(duration), eta_s);
            (void)snprintf(text, sizeof(text), "%.0f kt " LV_SYMBOL_BULLET " %s left",
                           (double)aircraft->ground_speed_kt, duration);
        } else {
            (void)snprintf(text, sizeof(text), "%.0f kt", (double)aircraft->ground_speed_kt);
        }
    } else {
        (void)snprintf(text, sizeof(text), "speed --");
    }
    set_label_if_changed(s_ui.fcs_row_text[1], text);

    if (aircraft != NULL) {
        char grouped[16];
        format_grouped(grouped, sizeof(grouped), lroundf(aircraft->distance_nm * scale));
        (void)snprintf(text, sizeof(text), "%s %s %s%s", grouped, unit,
                       cardinal_name(aircraft->bearing_deg),
                       UI_FOCUS_WIDE ? " of you" : "");
    } else {
        (void)snprintf(text, sizeof(text), was_airborne ? "position unknown"
                                                        : "not airborne yet");
    }
    set_label_if_changed(s_ui.fcs_row_text[2], text);

    if (schedule != NULL && (schedule->dep_gate[0] != '\0' || schedule->arr_gate[0] != '\0')) {
        (void)snprintf(text, sizeof(text), "Gate %s > %s",
                       schedule->dep_gate[0] != '\0' ? schedule->dep_gate : "--",
                       schedule->arr_gate[0] != '\0' ? schedule->arr_gate : "--");
    } else if (airframe[0] != '\0' || registration[0] != '\0') {
        /* Names are long; the registration fits beside one only when wide. */
        const bool both = airframe[0] != '\0' && registration[0] != '\0' &&
                          (UI_FOCUS_WIDE || airframe == type);
        (void)snprintf(text, sizeof(text), "%s%s%s", airframe[0] != '\0' ? airframe : registration,
                       both ? " " LV_SYMBOL_BULLET " " : "", both ? registration : "");
    } else {
        (void)snprintf(text, sizeof(text), "--");
    }
    set_label_if_changed(s_ui.fcs_row_text[3], text);

    if (aircraft != NULL) {
        char seen[16];
        format_age(seen, sizeof(seen), age);
        (void)snprintf(text, sizeof(text), live ? "live " LV_SYMBOL_BULLET " %s old"
                                                : "last seen %s ago", seen);
    } else if (was_airborne) {
        (void)snprintf(text, sizeof(text), landed ? "transponder off" : "no reports");
    } else {
        (void)snprintf(text, sizeof(text), "polled every %us",
                       (unsigned)settings->poll_interval_s);
    }
    set_label_if_changed(s_ui.fcs_row_text[4], text);

    for (size_t index = 0U; index < UI_FOCUS_ROWS; ++index) {
        lv_obj_set_style_text_color(s_ui.fcs_row_text[index], lv_color_hex(value_color), 0);
        if (index >= 3U) {
            lv_obj_set_style_text_color(s_ui.fcs_row_icon[index], lv_color_hex(accent), 0);
        } else {
            lv_obj_set_style_image_recolor(s_ui.fcs_row_icon[index], lv_color_hex(accent), 0);
        }
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
#if BOARD_HAS_BATTERY_SENSE
    update_battery_gauge(state->battery_valid, state->usb_present,
                         state->battery_percent);
#endif
    set_label_if_changed(s_ui.hdr_right, text);
    lv_obj_set_style_text_color(s_ui.hdr_right,
        lv_color_hex(since_success >= 0.0 && since_success <= UI_STALE_AGE_S
                         ? UI_COLOR_GREEN : UI_COLOR_AMBER), 0);

    /* A followed flight gets its own view while the feed is healthy; feed
     * problems keep the status screen, which says what is wrong. */
    const bool focused = state->settings->focus_flight[0] != '\0';
    const bool focus_view = focused &&
        (snapshot->aircraft_count > 0U || snapshot->state == AIRTRACK_FEED_EMPTY ||
         snapshot->state == AIRTRACK_FEED_SEARCHING ||
         snapshot->state == AIRTRACK_FEED_LIVE);
    show_group(s_ui.trk_focus, focus_view);
    if (focus_view) {
        set_radar_animation(false);
        show_group(s_ui.trk_data, false);
        show_group(s_ui.trk_empty, false);
        update_focus_view(state, since_success);
    } else if (snapshot->aircraft_count > 0U) {
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

        /* AIRFRAME • REGISTRATION (falling back to hex), or the emergency.
         * The airframe's name where known, else its ICAO designator. */
        if (aircraft->emergency) {
            (void)snprintf(text, sizeof(text), "EMERGENCY " LV_SYMBOL_BULLET " %s",
                           aircraft->squawk[0] != '\0' ? aircraft->squawk : "");
        } else {
            const char *type = aircraft->description[0] != '\0' ? aircraft->description
                                                                : aircraft->aircraft_type;
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
        if (aircraft->destination_valid && aircraft->route_confirmed) {
            float bearing_unused;
            airtrack_geometry(aircraft->latitude, aircraft->longitude,
                              aircraft->destination_latitude,
                              aircraft->destination_longitude,
                              &remaining_nm, &bearing_unused);
            if (aircraft->ground_speed_valid && aircraft->ground_speed_kt >= 60.0f) {
                eta_s = (long)(remaining_nm / aircraft->ground_speed_kt * 3600.0f);
            }
        }
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
        if (focused) {
            /* No radius applies to a followed flight; name it instead. */
            set_label_if_changed(s_ui.trk_empty_within, "following");
            set_label_if_changed(s_ui.trk_empty_radius, state->settings->focus_flight);
            set_label_if_changed(s_ui.trk_empty_unit, "");
        } else {
            (void)snprintf(text, sizeof(text), "%.0f", (double)radius);
            set_label_if_changed(s_ui.trk_empty_within, "within");
            set_label_if_changed(s_ui.trk_empty_radius, text);
            set_label_if_changed(s_ui.trk_empty_unit,
                                 unit_name(state->settings->distance_unit));
        }
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

    /* Attribution line, carrying the on-board climate where there is one. */
    if (state->environment_valid) {
        const bool fahrenheit =
            state->settings->temperature_unit == AIRTRACK_TEMPERATURE_F;
        const double shown = fahrenheit
            ? ((double)state->temperature_c * 9.0 / 5.0) + 32.0
            : (double)state->temperature_c;
        /* Battery lives in the header gauge, not here. */
        (void)snprintf(text, sizeof(text),
                       "adsb.fi " LV_SYMBOL_BULLET " %.1f%s "
                       LV_SYMBOL_BULLET " %.0f%%",
                       shown, fahrenheit ? "F" : "C",
                       (double)state->humidity_percent);
        set_label_if_changed(s_ui.trk_footer_data, text);
    }

    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    return ESP_OK;
}

esp_err_t ui_diagnostic_show_updating(const char *version, uint8_t percent,
                                      const char *phase, bool failed)
{
    if (!s_ui.initialized || version == NULL || phase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100U) {
        percent = 100U;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_ui.updating_visible) {
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        create_header(screen, UI_COLOR_AMBER, NULL);
        create_centered_label(screen, "UPDATING", 70, &lv_font_montserrat_20,
                              UI_COLOR_AMBER);
        s_ui.upd_version = create_centered_label(screen, "", 100,
                                                 &lv_font_montserrat_16,
                                                 UI_COLOR_TEXT);
        lv_obj_t *track = create_panel(screen, 16, 140, BOARD_LCD_H_RES - 32, 12,
                                       0x1C2A3D);
        lv_obj_set_style_radius(track, 6, 0);
        s_ui.upd_fill = create_panel(track, 0, 0, 1, 12, UI_COLOR_AMBER);
        lv_obj_set_style_radius(s_ui.upd_fill, 6, 0);
        s_ui.upd_percent = create_centered_label(screen, "0%", 160,
                                                 &lv_font_montserrat_28,
                                                 UI_COLOR_TEXT);
        s_ui.upd_phase = create_centered_label(screen, "", 204,
                                               &lv_font_montserrat_12,
                                               UI_COLOR_DIM);
        create_centered_label(screen, "Keep power connected", 250,
                              &lv_font_montserrat_12, UI_COLOR_MUTED);
        create_hline(screen, 14, 296, BOARD_LCD_H_RES - 28, 0x1C2A3D);
        create_centered_label(screen, "Data: adsb.fi", 304,
                              &lv_font_montserrat_10, UI_COLOR_MUTED);

        lv_obj_t *old_screen = s_ui.screen;
        lv_draw_buf_t *old_qr = s_ui.qr_draw_buffer;
        if (s_ui.tracking_visible && s_ui.radar_animating) {
            lv_anim_delete(s_ui.trk_radar_sweep, radar_sweep_animate);
            s_ui.radar_animating = false;
        }
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
        s_ui.diagnostic_visible = false;
        s_ui.tracking_visible = false;
        s_ui.updating_visible = true;
    }
    char text[48];
    (void)snprintf(text, sizeof(text), "AirTrack %s", version);
    set_label_if_changed(s_ui.upd_version, text);
    const int32_t width = ((int32_t)BOARD_LCD_H_RES - 32) * percent / 100;
    lv_obj_set_width(s_ui.upd_fill, width > 0 ? width : 1);
    lv_obj_set_style_bg_color(s_ui.upd_fill,
                              lv_color_hex(failed ? UI_COLOR_RED : UI_COLOR_AMBER), 0);
    (void)snprintf(text, sizeof(text), "%u%%", (unsigned)percent);
    set_label_if_changed(s_ui.upd_percent, text);
    set_label_if_changed(s_ui.upd_phase, phase);
    lv_obj_set_style_text_color(s_ui.upd_phase,
                                lv_color_hex(failed ? UI_COLOR_RED : UI_COLOR_DIM), 0);
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
