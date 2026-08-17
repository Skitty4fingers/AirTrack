#include "ui_diagnostic.h"

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
    lv_obj_t *tracking_mode;
    lv_obj_t *tracking_identity;
    lv_obj_t *tracking_meta;
    lv_obj_t *tracking_distance;
    lv_obj_t *tracking_bearing;
    lv_obj_t *tracking_altitude;
    lv_obj_t *tracking_speed;
    lv_obj_t *tracking_age;
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
    lv_obj_t *mode = create_label(header, "SETUP MODE", 10, 8, 152,
                                  lv_color_hex(0xFFB454));
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(canvas, qr_draw_buffer);
    lv_obj_set_size(canvas, UI_QR_CANVAS_SIZE, UI_QR_CANVAS_SIZE);
    lv_obj_set_pos(canvas, 16, 34);

    lv_obj_t *instruction = create_label(
        screen, "Scan QR to join Wi-Fi", 12, 178, 148,
        lv_color_hex(0xDDE7F4));
    lv_obj_set_style_text_align(instruction, LV_TEXT_ALIGN_CENTER, 0);

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
                                   const char *ip_address)
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
                                        &new_screen, &qr_canvas,
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
        return lv_color_hex(0x8BE36D);
    case AIRTRACK_FEED_EMPTY:
    case AIRTRACK_FEED_SEARCHING:
    case AIRTRACK_FEED_TIME_SYNC:
        return lv_color_hex(0x55D9F3);
    case AIRTRACK_FEED_STALE:
        return lv_color_hex(0xFFB454);
    case AIRTRACK_FEED_OFFLINE:
    case AIRTRACK_FEED_CONFIG_REQUIRED:
    default:
        return lv_color_hex(0xFF647C);
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

static void create_tracking_screen_locked(void)
{
    lv_obj_t *new_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(new_screen);
    lv_obj_set_style_bg_color(new_screen, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(new_screen, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(new_screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, BOARD_LCD_H_RES, 30);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    create_label(header, "AIRTRACK", 10, 8, 98, lv_color_hex(0x55D9F3));
    s_ui.tracking_mode = create_label(header, "SCAN", 110, 8, 52,
                                      lv_color_hex(0x55D9F3));
    lv_obj_set_style_text_align(s_ui.tracking_mode, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *card = lv_obj_create(new_screen);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 152, 196);
    lv_obj_set_pos(card, 10, 36);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101F31), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    s_ui.tracking_identity = create_label(card, "SEARCHING", 10, 10, 132,
                                          lv_color_hex(0xF2F6FC));
    s_ui.tracking_meta = create_label(card, "Waiting for adsb.fi", 10, 33, 132,
                                      lv_color_hex(0xA7B5CA));
    s_ui.tracking_distance = create_label(card, "--.- NM", 10, 62, 132,
                                          lv_color_hex(0x55D9F3));
    s_ui.tracking_bearing = create_label(card, "BRG --- TRUE", 10, 88, 132,
                                         lv_color_hex(0xDDE7F4));
    s_ui.tracking_altitude = create_label(card, "ALT --", 10, 116, 132,
                                          lv_color_hex(0xDDE7F4));
    s_ui.tracking_speed = create_label(card, "SPD --", 10, 140, 132,
                                       lv_color_hex(0xDDE7F4));
    s_ui.tracking_age = create_label(card, "Waiting for a valid response", 10,
                                     169, 132, lv_color_hex(0x7F8FA8));

    lv_obj_t *footer = lv_obj_create(new_screen);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, BOARD_LCD_H_RES, 82);
    lv_obj_set_pos(footer, 0, 238);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0D1A2B), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    s_ui.ssid = create_label(footer, "SSID: --", 10, 8, 152,
                             lv_color_hex(0xDDE7F4));
    s_ui.ip = create_label(footer, "IP: --", 10, 29, 152,
                           lv_color_hex(0xDDE7F4));
    create_label(footer, "Data: adsb.fi", 10, 52, 152,
                 lv_color_hex(0x55D9F3));
    create_label(footer, "Not for navigation", 10, 68, 152,
                 lv_color_hex(0x6F819B));

    lv_obj_t *old_screen = s_ui.screen;
    lv_draw_buf_t *old_qr = s_ui.qr_draw_buffer;
    lv_screen_load(new_screen);
    s_ui.screen = new_screen;
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
    s_ui.diagnostic_visible = false;
    s_ui.tracking_visible = true;
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
    lv_label_set_text(s_ui.tracking_mode, tracking_mode_text(snapshot->state));
    lv_obj_set_style_text_color(s_ui.tracking_mode,
                                tracking_state_color(snapshot->state), 0);
    char text[128];
    if (snapshot->aircraft_count > 0U) {
        const airtrack_aircraft_t *aircraft = &snapshot->aircraft[0];
        lv_label_set_text(s_ui.tracking_identity, target_identity(aircraft));
        (void)snprintf(text, sizeof(text), "%s%s%s",
                       aircraft->registration,
                       aircraft->registration[0] != '\0' &&
                               aircraft->aircraft_type[0] != '\0' ? " · " : "",
                       aircraft->aircraft_type);
        lv_label_set_text(s_ui.tracking_meta, text[0] != '\0' ? text : aircraft->hex);

        float distance = aircraft->distance_nm;
        const char *unit = "NM";
        if (state->settings->distance_unit == AIRTRACK_DISTANCE_KM) {
            distance *= 1.852f;
            unit = "km";
        } else if (state->settings->distance_unit == AIRTRACK_DISTANCE_MI) {
            distance *= 1.150779f;
            unit = "mi";
        }
        (void)snprintf(text, sizeof(text), "%.1f %s", (double)distance, unit);
        lv_label_set_text(s_ui.tracking_distance, text);
        (void)snprintf(text, sizeof(text), "BRG %03.0f° TRUE",
                       (double)aircraft->bearing_deg);
        lv_label_set_text(s_ui.tracking_bearing, text);
        if (aircraft->ground) {
            memcpy(text, "ALT GROUND", sizeof("ALT GROUND"));
        } else if (aircraft->altitude_valid) {
            if (aircraft->vertical_rate_valid) {
                (void)snprintf(text, sizeof(text), "ALT %ld ft  VR %+ld",
                               (long)aircraft->altitude_ft,
                               (long)aircraft->vertical_rate_fpm);
            } else {
                (void)snprintf(text, sizeof(text), "ALT %ld ft",
                               (long)aircraft->altitude_ft);
            }
        } else {
            memcpy(text, "ALT --", sizeof("ALT --"));
        }
        lv_label_set_text(s_ui.tracking_altitude, text);
        if (aircraft->ground_speed_valid) {
            if (aircraft->track_valid) {
                (void)snprintf(text, sizeof(text), "SPD %.0f kt  TRK %03.0f°",
                               (double)aircraft->ground_speed_kt,
                               (double)aircraft->track_deg);
            } else {
                (void)snprintf(text, sizeof(text), "SPD %.0f kt",
                               (double)aircraft->ground_speed_kt);
            }
        } else {
            memcpy(text, "SPD --", sizeof("SPD --"));
        }
        lv_label_set_text(s_ui.tracking_speed, text);
        double age = aircraft->seen_pos_s;
        if (snapshot->updated_monotonic_ms > 0) {
            const int64_t elapsed = (esp_timer_get_time() / 1000LL) -
                                    snapshot->updated_monotonic_ms;
            if (elapsed > 0) {
                age += (double)elapsed / 1000.0;
            }
        }
        (void)snprintf(text, sizeof(text), "%s · position %.0fs old",
                       tracking_mode_text(snapshot->state), age);
        lv_label_set_text(s_ui.tracking_age, text);
    } else {
        const char *headline = snapshot->state == AIRTRACK_FEED_EMPTY
                                   ? "NO AIRCRAFT"
                               : snapshot->state == AIRTRACK_FEED_TIME_SYNC
                                   ? "SYNCING TIME"
                               : snapshot->state == AIRTRACK_FEED_CONFIG_REQUIRED
                                   ? "SETUP REQUIRED"
                               : snapshot->state == AIRTRACK_FEED_OFFLINE
                                   ? "FEED OFFLINE" : "SEARCHING";
        lv_label_set_text(s_ui.tracking_identity, headline);
        (void)snprintf(text, sizeof(text), "Within %u NM",
                       (unsigned)state->settings->radius_nm);
        lv_label_set_text(s_ui.tracking_meta, text);
        lv_label_set_text(s_ui.tracking_distance, "--.- NM");
        lv_label_set_text(s_ui.tracking_bearing, "BRG --- TRUE");
        lv_label_set_text(s_ui.tracking_altitude, "ALT --");
        lv_label_set_text(s_ui.tracking_speed, "SPD --");
        lv_label_set_text(s_ui.tracking_age,
                          airtrack_feed_error_name(snapshot->error));
    }

    (void)snprintf(text, sizeof(text), "SSID: %s", state->ssid);
    lv_label_set_text(s_ui.ssid, text);
    (void)snprintf(text, sizeof(text), "IP: %s", state->ip_address);
    lv_label_set_text(s_ui.ip, text);
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
