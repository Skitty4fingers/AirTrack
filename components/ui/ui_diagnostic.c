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

#define UI_FLUSH_NOTIFY_INDEX 1U
#define UI_FLUSH_TIMEOUT_MS 1000U
#define UI_QR_CANVAS_SIZE 140U
#define UI_QR_QUIET_ZONE_MODULES 4U
#define UI_QR_MAX_VERSION 11
#define UI_WIFI_SSID_MAX_BYTES 32U
#define UI_WIFI_PASSWORD_MIN_BYTES 8U
#define UI_WIFI_PASSWORD_MAX_BYTES 63U
#define UI_WIFI_QR_PAYLOAD_BYTES 224U
#define UI_ARROW_BOX 44
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
    lv_obj_t *trk_pill;
    lv_obj_t *trk_pill_text;
    lv_obj_t *trk_data;          /* container shown while a target exists */
    lv_obj_t *trk_identity;
    lv_obj_t *trk_meta;
    lv_obj_t *trk_distance;
    lv_obj_t *trk_unit;
    lv_obj_t *trk_ring;
    lv_obj_t *trk_arrow;
    lv_obj_t *trk_bearing;
    lv_obj_t *trk_bearing_sub;
    lv_obj_t *trk_alt;
    lv_obj_t *trk_vs;
    lv_obj_t *trk_gs;
    lv_obj_t *trk_sqk;
    lv_obj_t *trk_empty;         /* container shown without a target */
    lv_obj_t *trk_empty_head;
    lv_obj_t *trk_empty_sub;
    lv_obj_t *trk_empty_hint;
    lv_obj_t *trk_age;
    lv_obj_t *trk_wifi_icon;
    lv_obj_t *trk_ssid;
    lv_obj_t *trk_rssi;
    lv_obj_t *trk_ip;
    lv_point_precise_t arrow_points[5];
    lv_draw_buf_t *qr_draw_buffer;
} ui_context_t;

typedef struct {
    lv_obj_t *canvas;
    bool rendered;
} qr_render_context_t;

static ui_context_t s_ui;
static volatile TaskHandle_t s_flush_waiter;

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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, BOARD_LCD_H_RES, 30);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_t *mode = create_label(header,
                                  recovery ? "RECOVERY SETUP" : "SETUP MODE",
                                  10, 8, 152,
                                  lv_color_hex(recovery ? UI_COLOR_RED
                                                        : UI_COLOR_AMBER));
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(canvas, qr_draw_buffer);
    lv_obj_set_size(canvas, UI_QR_CANVAS_SIZE, UI_QR_CANVAS_SIZE);
    lv_obj_set_pos(canvas, 16, 34);

    lv_obj_t *instruction = create_label(
        screen,
        recovery ? "Retrying Wi-Fi or scan QR"
                 : "Scan QR to join Wi-Fi",
        6, 178, 160, lv_color_hex(0xDDE7F4));
    lv_obj_set_style_text_align(instruction, LV_TEXT_ALIGN_CENTER, 0);
    if (recovery) {
        lv_obj_set_style_text_font(instruction, &lv_font_montserrat_12, 0);
    }

    char label_text[96];
    (void)snprintf(label_text, sizeof(label_text), "SSID: %s", ap_ssid);
    lv_obj_t *credential_ssid = create_label(
        screen, label_text, 10, 198, 152, lv_color_hex(0xF2F6FC));
    lv_label_set_long_mode(credential_ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(credential_ssid, 5000, 0);

    (void)snprintf(label_text, sizeof(label_text), "Password: %s",
                   ap_password);
    lv_obj_t *credential_password = create_label(
        screen, label_text, 10, 217, 152, lv_color_hex(0xFFB454));
    lv_label_set_long_mode(credential_password, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(credential_password, 6000, 0);

    lv_obj_t *footer = lv_obj_create(screen);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, BOARD_LCD_H_RES, 76);
    lv_obj_set_pos(footer, 0, 244);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    create_label(footer, "NETWORK / AP", 10, 4, 152,
                 lv_color_hex(0x6F819B));
    (void)snprintf(label_text, sizeof(label_text), "SSID: %s", ap_ssid);
    lv_obj_t *footer_ssid = create_label(
        footer, label_text, 10, 22, 152, lv_color_hex(0xDDE7F4));
    lv_label_set_long_mode(footer_ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(footer_ssid, 5000, 0);
    (void)snprintf(label_text, sizeof(label_text), "IP: %s", ip_address);
    create_label(footer, label_text, 10, 40, 152, lv_color_hex(0xDDE7F4));
    create_label(footer, "Data: adsb.fi", 10, 58, 152,
                 lv_color_hex(0x55D9F3));

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

static const char *tracking_mode_text(airtrack_feed_state_t state)
{
    switch (state) {
    case AIRTRACK_FEED_LIVE:
        return "LIVE";
    case AIRTRACK_FEED_EMPTY:
        return "CLEAR";
    case AIRTRACK_FEED_STALE:
        return "STALE";
    case AIRTRACK_FEED_TIME_SYNC:
        return "TIME";
    case AIRTRACK_FEED_SEARCHING:
        return "SCAN";
    case AIRTRACK_FEED_CONFIG_REQUIRED:
        return "SETUP";
    case AIRTRACK_FEED_OFFLINE:
    default:
        return "OFFLINE";
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

static lv_obj_t *create_font_label(lv_obj_t *parent, const char *text,
                                   int32_t x, int32_t y, int32_t width,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = create_label(parent, text, x, y, width,
                                   lv_color_hex(color));
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height, uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
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

static void set_arrow_bearing(float bearing_deg)
{
    const float radians = bearing_deg * (3.14159265f / 180.0f);
    const float cx = UI_ARROW_BOX / 2.0f;
    const float cy = UI_ARROW_BOX / 2.0f;
    const float tip = 17.0f;
    const float wing = 11.0f;
    const float notch = 5.0f;
    const float wing_angle = 150.0f * (3.14159265f / 180.0f);
    lv_point_precise_t *points = s_ui.arrow_points;
    points[0].x = cx + tip * sinf(radians);
    points[0].y = cy - tip * cosf(radians);
    points[1].x = cx + wing * sinf(radians + wing_angle);
    points[1].y = cy - wing * cosf(radians + wing_angle);
    points[2].x = cx - notch * sinf(radians);
    points[2].y = cy + notch * cosf(radians);
    points[3].x = cx + wing * sinf(radians - wing_angle);
    points[3].y = cy - wing * cosf(radians - wing_angle);
    points[4] = points[0];
    lv_line_set_points(s_ui.trk_arrow, s_ui.arrow_points, 5);
    lv_obj_invalidate(s_ui.trk_ring);
}

static void create_tracking_screen_locked(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Header: wordmark and state pill. */
    lv_obj_t *header = create_panel(screen, 0, 0, BOARD_LCD_H_RES, 26,
                                    UI_COLOR_PANEL);
    create_font_label(header, "AIRTRACK", 8, 7, 90, &lv_font_montserrat_12,
                      UI_COLOR_CYAN);
    s_ui.trk_pill = create_panel(header, 108, 5, 56, 16, UI_COLOR_CYAN);
    lv_obj_set_style_radius(s_ui.trk_pill, 8, 0);
    s_ui.trk_pill_text = create_font_label(s_ui.trk_pill, "SCAN", 0, 2, 56,
                                           &lv_font_montserrat_10,
                                           UI_COLOR_BG);
    lv_obj_set_style_text_align(s_ui.trk_pill_text, LV_TEXT_ALIGN_CENTER, 0);

    /* Target block. */
    s_ui.trk_data = create_group(screen, 0, 26, BOARD_LCD_H_RES, 224);
    s_ui.trk_identity = create_font_label(s_ui.trk_data, "--", 8, 4, 156,
                                          &lv_font_montserrat_28,
                                          UI_COLOR_TEXT);
    s_ui.trk_meta = create_font_label(s_ui.trk_data, "", 8, 38, 156,
                                      &lv_font_montserrat_12, UI_COLOR_DIM);
    s_ui.trk_distance = create_font_label(s_ui.trk_data, "--", 6, 54, 116,
                                          &lv_font_montserrat_40,
                                          UI_COLOR_CYAN);
    s_ui.trk_unit = create_font_label(s_ui.trk_data, "NM", 122, 76, 44,
                                      &lv_font_montserrat_16, UI_COLOR_DIM);

    s_ui.trk_ring = create_group(s_ui.trk_data, 8, 106, UI_ARROW_BOX,
                                 UI_ARROW_BOX);
    lv_obj_set_style_radius(s_ui.trk_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.trk_ring, 1, 0);
    lv_obj_set_style_border_color(s_ui.trk_ring, lv_color_hex(0x2A3B55), 0);
    lv_obj_set_style_border_opa(s_ui.trk_ring, LV_OPA_COVER, 0);
    s_ui.trk_arrow = lv_line_create(s_ui.trk_ring);
    lv_obj_remove_style_all(s_ui.trk_arrow);
    lv_obj_set_pos(s_ui.trk_arrow, 0, 0);
    lv_obj_set_size(s_ui.trk_arrow, UI_ARROW_BOX, UI_ARROW_BOX);
    lv_obj_set_style_line_width(s_ui.trk_arrow, 3, 0);
    lv_obj_set_style_line_rounded(s_ui.trk_arrow, true, 0);
    lv_obj_set_style_line_color(s_ui.trk_arrow, lv_color_hex(UI_COLOR_GREEN), 0);
    set_arrow_bearing(0.0f);

    s_ui.trk_bearing = create_font_label(s_ui.trk_data, "---° --", 60, 108,
                                         104, &lv_font_montserrat_16,
                                         UI_COLOR_TEXT);
    s_ui.trk_bearing_sub = create_font_label(s_ui.trk_data, "bearing", 60, 130,
                                             104, &lv_font_montserrat_12,
                                             UI_COLOR_MUTED);

    create_font_label(s_ui.trk_data, "ALT", 8, 158, 76, &lv_font_montserrat_10,
                      UI_COLOR_MUTED);
    create_font_label(s_ui.trk_data, "V/S", 92, 158, 72, &lv_font_montserrat_10,
                      UI_COLOR_MUTED);
    s_ui.trk_alt = create_font_label(s_ui.trk_data, "--", 8, 170, 80,
                                     &lv_font_montserrat_16, UI_COLOR_TEXT);
    s_ui.trk_vs = create_font_label(s_ui.trk_data, "--", 92, 170, 74,
                                    &lv_font_montserrat_16, UI_COLOR_TEXT);
    create_font_label(s_ui.trk_data, "GS", 8, 192, 76, &lv_font_montserrat_10,
                      UI_COLOR_MUTED);
    create_font_label(s_ui.trk_data, "SQUAWK", 92, 192, 72,
                      &lv_font_montserrat_10, UI_COLOR_MUTED);
    s_ui.trk_gs = create_font_label(s_ui.trk_data, "--", 8, 204, 80,
                                    &lv_font_montserrat_16, UI_COLOR_TEXT);
    s_ui.trk_sqk = create_font_label(s_ui.trk_data, "--", 92, 204, 74,
                                     &lv_font_montserrat_16, UI_COLOR_TEXT);

    /* Empty / waiting block. */
    s_ui.trk_empty = create_group(screen, 0, 26, BOARD_LCD_H_RES, 224);
    s_ui.trk_empty_head = create_font_label(s_ui.trk_empty, "SEARCHING", 6,
                                            76, 160, &lv_font_montserrat_20,
                                            UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_ui.trk_empty_head, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_empty_sub = create_font_label(s_ui.trk_empty, "", 8, 104, 156,
                                           &lv_font_montserrat_12,
                                           UI_COLOR_DIM);
    lv_obj_set_style_text_align(s_ui.trk_empty_sub, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.trk_empty_hint = create_font_label(s_ui.trk_empty, "", 8, 122, 156,
                                            &lv_font_montserrat_12,
                                            UI_COLOR_MUTED);
    lv_obj_set_style_text_align(s_ui.trk_empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_ui.trk_empty, LV_OBJ_FLAG_HIDDEN);

    s_ui.trk_age = create_font_label(screen, "", 8, 250, 156,
                                     &lv_font_montserrat_12, UI_COLOR_MUTED);

    /* Footer: network and attribution. */
    lv_obj_t *footer = create_panel(screen, 0, 266, BOARD_LCD_H_RES, 54,
                                    UI_COLOR_PANEL);
    s_ui.trk_wifi_icon = create_font_label(footer, LV_SYMBOL_WIFI, 8, 5, 16,
                                           &lv_font_montserrat_12,
                                           UI_COLOR_GREEN);
    s_ui.trk_ssid = create_font_label(footer, "--", 26, 5, 84,
                                      &lv_font_montserrat_12, UI_COLOR_TEXT);
    s_ui.trk_rssi = create_font_label(footer, "", 110, 6, 54,
                                      &lv_font_montserrat_10, UI_COLOR_DIM);
    lv_obj_set_style_text_align(s_ui.trk_rssi, LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.trk_ip = create_font_label(footer, "IP --", 8, 21, 156,
                                    &lv_font_montserrat_12, UI_COLOR_TEXT);
    create_font_label(footer, "Data: adsb.fi", 8, 38, 70,
                      &lv_font_montserrat_10, UI_COLOR_CYAN);
    lv_obj_t *notice = create_font_label(footer, "not for navigation", 62, 38,
                                         102, &lv_font_montserrat_10,
                                         UI_COLOR_MUTED);
    lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_RIGHT, 0);

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
    char text[96];

    /* Header pill. */
    set_label_if_changed(s_ui.trk_pill_text, tracking_mode_text(snapshot->state));
    lv_obj_set_style_bg_color(s_ui.trk_pill,
                              tracking_state_color(snapshot->state), 0);

    /* Effective position age: seconds since the report plus time since the
     * last successful poll (never a failure or waiting publish). */
    double age = 0.0;
    if (snapshot->aircraft_count > 0U) {
        age = snapshot->aircraft[0].seen_pos_s;
        if (snapshot->last_success_monotonic_ms > 0) {
            const int64_t elapsed = (esp_timer_get_time() / 1000LL) -
                                    snapshot->last_success_monotonic_ms;
            if (elapsed > 0) {
                age += (double)elapsed / 1000.0;
            }
        }
    }

    if (snapshot->aircraft_count > 0U) {
        const airtrack_aircraft_t *aircraft = &snapshot->aircraft[0];
        show_group(s_ui.trk_empty, false);
        show_group(s_ui.trk_data, true);
        const bool dimmed = snapshot->state != AIRTRACK_FEED_LIVE ||
                            age > UI_STALE_AGE_S;
        const uint32_t value_color = dimmed ? UI_COLOR_DIM : UI_COLOR_TEXT;
        const uint32_t accent = aircraft->emergency ? UI_COLOR_RED
                                : dimmed ? UI_COLOR_AMBER : UI_COLOR_CYAN;

        set_label_if_changed(s_ui.trk_identity, target_identity(aircraft));
        lv_obj_set_style_text_color(s_ui.trk_identity,
                                    lv_color_hex(value_color), 0);

        /* registration · type · category, whichever exist */
        size_t used = 0U;
        text[0] = '\0';
        const char *parts[3] = {
            aircraft->registration[0] != '\0' &&
                    strcmp(aircraft->registration,
                           target_identity(aircraft)) != 0
                ? aircraft->registration : "",
            aircraft->aircraft_type,
            aircraft->description[0] != '\0' ? "" : aircraft->hex,
        };
        for (size_t index = 0U; index < 3U; ++index) {
            if (parts[index][0] == '\0') {
                continue;
            }
            const int written = snprintf(text + used, sizeof(text) - used,
                                         "%s%s", used > 0U ? " " LV_SYMBOL_BULLET " " : "",
                                         parts[index]);
            if (written < 0 || (size_t)written >= sizeof(text) - used) {
                break;
            }
            used += (size_t)written;
        }
        if (aircraft->emergency) {
            (void)snprintf(text, sizeof(text), "EMERGENCY %s",
                           aircraft->squawk[0] != '\0' ? aircraft->squawk : "");
        }
        set_label_if_changed(s_ui.trk_meta, text[0] != '\0' ? text : aircraft->hex);
        lv_obj_set_style_text_color(s_ui.trk_meta,
            lv_color_hex(aircraft->emergency ? UI_COLOR_RED : UI_COLOR_DIM), 0);

        float distance = aircraft->distance_nm;
        const char *unit = "NM";
        if (state->settings->distance_unit == AIRTRACK_DISTANCE_KM) {
            distance *= 1.852f;
            unit = "km";
        } else if (state->settings->distance_unit == AIRTRACK_DISTANCE_MI) {
            distance *= 1.150779f;
            unit = "mi";
        }
        (void)snprintf(text, sizeof(text), distance < 100.0f ? "%.1f" : "%.0f",
                       (double)distance);
        set_label_if_changed(s_ui.trk_distance, text);
        lv_obj_set_style_text_color(s_ui.trk_distance, lv_color_hex(accent), 0);
        set_label_if_changed(s_ui.trk_unit, unit);

        set_arrow_bearing(aircraft->bearing_deg);
        lv_obj_set_style_line_color(s_ui.trk_arrow, lv_color_hex(accent), 0);
        (void)snprintf(text, sizeof(text), "%03.0f° %s",
                       (double)aircraft->bearing_deg,
                       cardinal_name(aircraft->bearing_deg));
        set_label_if_changed(s_ui.trk_bearing, text);
        lv_obj_set_style_text_color(s_ui.trk_bearing,
                                    lv_color_hex(value_color), 0);
        if (aircraft->track_valid) {
            (void)snprintf(text, sizeof(text), "bearing " LV_SYMBOL_BULLET " trk %03.0f°",
                           (double)aircraft->track_deg);
        } else {
            (void)snprintf(text, sizeof(text), "bearing from here");
        }
        set_label_if_changed(s_ui.trk_bearing_sub, text);

        if (aircraft->ground) {
            (void)snprintf(text, sizeof(text), "GROUND");
        } else if (aircraft->altitude_valid) {
            char grouped[16];
            format_grouped(grouped, sizeof(grouped), aircraft->altitude_ft);
            (void)snprintf(text, sizeof(text), "%s ft", grouped);
        } else {
            (void)snprintf(text, sizeof(text), "--");
        }
        set_label_if_changed(s_ui.trk_alt, text);
        lv_obj_set_style_text_color(s_ui.trk_alt, lv_color_hex(value_color), 0);

        if (aircraft->vertical_rate_valid && !aircraft->ground) {
            const long rate = aircraft->vertical_rate_fpm;
            char grouped[16];
            format_grouped(grouped, sizeof(grouped), rate < 0 ? -rate : rate);
            (void)snprintf(text, sizeof(text), "%s%s",
                           rate > 0 ? LV_SYMBOL_UP " "
                           : rate < 0 ? LV_SYMBOL_DOWN " " : "",
                           rate == 0 ? "level" : grouped);
        } else {
            (void)snprintf(text, sizeof(text), "--");
        }
        set_label_if_changed(s_ui.trk_vs, text);
        lv_obj_set_style_text_color(s_ui.trk_vs, lv_color_hex(value_color), 0);

        if (aircraft->ground_speed_valid) {
            (void)snprintf(text, sizeof(text), "%.0f kt",
                           (double)aircraft->ground_speed_kt);
        } else {
            (void)snprintf(text, sizeof(text), "--");
        }
        set_label_if_changed(s_ui.trk_gs, text);
        lv_obj_set_style_text_color(s_ui.trk_gs, lv_color_hex(value_color), 0);

        set_label_if_changed(s_ui.trk_sqk,
                             aircraft->squawk[0] != '\0' ? aircraft->squawk
                                                          : "--");
        lv_obj_set_style_text_color(s_ui.trk_sqk,
            lv_color_hex(aircraft->emergency ? UI_COLOR_RED : value_color), 0);

        if (age < 60.0) {
            (void)snprintf(text, sizeof(text), "seen %.0f s ago " LV_SYMBOL_BULLET " %lu nearby",
                           age, (unsigned long)snapshot->aircraft_accepted);
        } else {
            (void)snprintf(text, sizeof(text), "seen %.0f min ago " LV_SYMBOL_BULLET " %s",
                           age / 60.0,
                           snapshot->error != AIRTRACK_ERROR_NONE
                               ? airtrack_feed_error_name(snapshot->error)
                               : "waiting");
        }
        set_label_if_changed(s_ui.trk_age, text);
        lv_obj_set_style_text_color(s_ui.trk_age,
            lv_color_hex(dimmed ? UI_COLOR_AMBER : UI_COLOR_MUTED), 0);
    } else {
        show_group(s_ui.trk_data, false);
        show_group(s_ui.trk_empty, true);
        const char *headline;
        const char *hint;
        switch (snapshot->state) {
        case AIRTRACK_FEED_EMPTY:
            headline = "NO AIRCRAFT";
            hint = "sky is clear right now";
            break;
        case AIRTRACK_FEED_TIME_SYNC:
            headline = "SYNCING TIME";
            hint = "waiting for network time";
            break;
        case AIRTRACK_FEED_CONFIG_REQUIRED:
            headline = "SET LOCATION";
            hint = "open the IP below in a browser";
            break;
        case AIRTRACK_FEED_OFFLINE:
            headline = state->wifi_connected ? "FEED OFFLINE" : "NO WI-FI";
            hint = state->wifi_connected
                       ? airtrack_feed_error_name(snapshot->error)
                       : "reconnecting to Wi-Fi";
            break;
        default:
            headline = "SEARCHING";
            hint = "requesting nearby traffic";
            break;
        }
        set_label_if_changed(s_ui.trk_empty_head, headline);
        lv_obj_set_style_text_color(s_ui.trk_empty_head,
                                    tracking_state_color(snapshot->state), 0);
        (void)snprintf(text, sizeof(text), "within %u NM",
                       (unsigned)state->settings->radius_nm);
        set_label_if_changed(s_ui.trk_empty_sub, text);
        set_label_if_changed(s_ui.trk_empty_hint, hint);
        if (snapshot->state == AIRTRACK_FEED_OFFLINE &&
            snapshot->retry_after_s > 0U && state->wifi_connected) {
            (void)snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " retry in %lus",
                           airtrack_feed_error_name(snapshot->error),
                           (unsigned long)snapshot->retry_after_s);
        } else {
            (void)snprintf(text, sizeof(text), "%lu reports last poll",
                           (unsigned long)snapshot->aircraft_reported);
            if (snapshot->last_success_monotonic_ms == 0) {
                text[0] = '\0';
            }
        }
        set_label_if_changed(s_ui.trk_age, text);
        lv_obj_set_style_text_color(s_ui.trk_age, lv_color_hex(UI_COLOR_MUTED), 0);
    }

    /* Footer. */
    set_label_if_changed(s_ui.trk_ssid, state->ssid);
    lv_obj_set_style_text_color(s_ui.trk_wifi_icon,
        lv_color_hex(state->wifi_connected ? UI_COLOR_GREEN : UI_COLOR_RED), 0);
    if (state->wifi_connected && state->rssi_available) {
        (void)snprintf(text, sizeof(text), "%d dBm", (int)state->rssi_dbm);
    } else if (state->wifi_connected) {
        (void)snprintf(text, sizeof(text), "linked");
    } else {
        (void)snprintf(text, sizeof(text), "offline");
    }
    set_label_if_changed(s_ui.trk_rssi, text);
    (void)snprintf(text, sizeof(text), "IP %s", state->ip_address);
    set_label_if_changed(s_ui.trk_ip, text);

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
