#include "status_web.h"

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "ota_update.h"
#include "storage_logger.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define STATUS_WEB_VERSION_MAX_BYTES 32U
#define STATUS_WEB_HOST_MAX_BYTES \
    (STATUS_WEB_IPV4_TEXT_MAX_BYTES + sizeof(":80"))
#define STATUS_WEB_LOCATION_MAX_BYTES \
    (sizeof("http://") - 1U + STATUS_WEB_IPV4_TEXT_MAX_BYTES + \
     sizeof("/api/v1/aircraft"))
#define STATUS_WEB_CSRF_BYTES 32U
#define STATUS_WEB_FORM_MAX_BYTES 512U

static const char *TAG = "status_web";

typedef struct {
    char ssid[STATUS_WEB_SSID_MAX_BYTES + 1U];
    char ip_address[STATUS_WEB_IPV4_TEXT_MAX_BYTES + 1U];
    bool rssi_available;
    int8_t rssi_dbm;
    bool sd_mounted;
    bool sd_logging_enabled;
    uint32_t sd_records_written;
    uint64_t sd_log_bytes;
    uint32_t sd_log_files;
    uint32_t sd_files_pruned;
    uint32_t flash_bytes;
    uint32_t uptime_s;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    bool time_synchronized;
    bool night;
    int local_minutes;
    uint32_t polls_ok;
    uint32_t polls_failed;
    uint32_t tls_connections;
    airtrack_settings_t settings;
    airtrack_snapshot_t aircraft;
} status_web_snapshot_storage_t;

static httpd_handle_t s_server;
static status_web_snapshot_storage_t s_snapshot;
static status_web_save_settings_cb_t s_save_settings;
static status_web_reboot_cb_t s_reboot;
static status_web_factory_reset_cb_t s_factory_reset;
static void *s_user_context;
static char s_csrf_token[STATUS_WEB_CSRF_BYTES + 1U];
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

/* HTTP start/stop may block, so lifecycle calls use a mutex rather than the
 * short critical section used to copy a request snapshot. */
static StaticSemaphore_t s_lifecycle_lock_storage;
static SemaphoreHandle_t s_lifecycle_lock;
static portMUX_TYPE s_lifecycle_lock_init = portMUX_INITIALIZER_UNLOCKED;

static void make_csrf_token(void)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[STATUS_WEB_CSRF_BYTES / 2U];
    esp_fill_random(random, sizeof(random));
    for (size_t index = 0U; index < sizeof(random); ++index) {
        s_csrf_token[index * 2U] = hex[random[index] >> 4U];
        s_csrf_token[(index * 2U) + 1U] = hex[random[index] & 0x0fU];
    }
    s_csrf_token[STATUS_WEB_CSRF_BYTES] = '\0';
    memset(random, 0, sizeof(random));
}

extern const char app_css_start[] asm("_binary_app_css_start");
extern const char app_css_end[] asm("_binary_app_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

/* Inline SVG icons (Material-style paths), fill = currentColor. */
#define ICON_PLANE "<svg viewBox=\"0 0 24 24\"><path d=\"M21 16v-2l-8-5V3.5a1.5 1.5 0 0 0-3 0V9l-8 5v2l8-2.5V19l-2 1.5V22l3.5-1 3.5 1v-1.5L13 19v-5.5z\"/></svg>"
#define ICON_HOME "<svg viewBox=\"0 0 24 24\"><path d=\"M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z\"/></svg>"
#define ICON_PIN "<svg viewBox=\"0 0 24 24\"><path d=\"M12 2a7 7 0 0 0-7 7c0 5.2 7 13 7 13s7-7.8 7-13a7 7 0 0 0-7-7zm0 9.5A2.5 2.5 0 1 1 12 6.5a2.5 2.5 0 0 1 0 5z\"/></svg>"
#define ICON_MONITOR "<svg viewBox=\"0 0 24 24\"><path d=\"M21 2H3a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h7v2H8v2h8v-2h-2v-2h7a2 2 0 0 0 2-2V4a2 2 0 0 0-2-2zm0 14H3V4h18z\"/></svg>"
#define ICON_SD "<svg viewBox=\"0 0 24 24\"><path d=\"M18 2h-8L4 8v12a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V4a2 2 0 0 0-2-2zm-6 6H10V4h2zm3 0h-2V4h2zm3 0h-2V4h2z\"/></svg>"
#define ICON_GEAR "<svg viewBox=\"0 0 24 24\"><path d=\"M19.4 13a7.6 7.6 0 0 0 0-2l2.1-1.6a.5.5 0 0 0 .1-.6l-2-3.5a.5.5 0 0 0-.6-.2l-2.5 1a7.3 7.3 0 0 0-1.7-1l-.4-2.6a.5.5 0 0 0-.5-.5h-4a.5.5 0 0 0-.5.4L9 5.1a7.3 7.3 0 0 0-1.7 1l-2.5-1a.5.5 0 0 0-.6.2l-2 3.5a.5.5 0 0 0 .1.6L4.5 11a7.6 7.6 0 0 0 0 2l-2.1 1.6a.5.5 0 0 0-.1.6l2 3.5c.1.2.4.3.6.2l2.5-1a7.3 7.3 0 0 0 1.7 1l.4 2.6c0 .3.2.5.5.5h4c.3 0 .5-.2.5-.5l.4-2.6a7.3 7.3 0 0 0 1.7-1l2.5 1c.2.1.5 0 .6-.2l2-3.5a.5.5 0 0 0-.1-.6zM12 15.5a3.5 3.5 0 1 1 0-7 3.5 3.5 0 0 1 0 7z\"/></svg>"
#define ICON_WIFI "<svg viewBox=\"0 0 24 24\"><path d=\"M12 21l3.5-4.7a5.9 5.9 0 0 0-7 0zm0-9a10 10 0 0 0-6.4 2.3l1.8 2.4a7 7 0 0 1 9.2 0l1.8-2.4A10 10 0 0 0 12 12zm0-6A16 16 0 0 0 1.5 9.9l1.8 2.4a13 13 0 0 1 17.4 0l1.8-2.4A16 16 0 0 0 12 6z\"/></svg>"
#define ICON_CLOUD "<svg viewBox=\"0 0 24 24\"><path d=\"M19.4 10A7.5 7.5 0 0 0 5.4 8 6 6 0 0 0 6 20h13a5 5 0 0 0 .4-10z\"/></svg>"
#define ICON_CLOCK "<svg viewBox=\"0 0 24 24\"><path d=\"M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 18a8 8 0 1 1 0-16 8 8 0 0 1 0 16zm.5-13H11v6l5.2 3.2.8-1.3-4.5-2.7z\"/></svg>"
#define ICON_NAV "<svg viewBox=\"0 0 24 24\"><path d=\"M21 3 3 10.5v1l7.5 2 2 7.5h1z\"/></svg>"
#define ICON_MTN "<svg viewBox=\"0 0 24 24\"><path d=\"m14 6-3.8 5 2.9 3.8-1.6 1.2C9.6 13.5 7 10 7 10l-6 8h22z\"/></svg>"
#define ICON_GAUGE "<svg viewBox=\"0 0 24 24\"><path d=\"M12 4a10 10 0 0 0-8.7 15h17.4A10 10 0 0 0 12 4zm0 2a8 8 0 0 1 8 8 8 8 0 0 1-.9 3.7L13 13.5a1.5 1.5 0 0 0-1.9-1.9L7.7 8.2A7.9 7.9 0 0 1 12 6zM6.4 9.5l3.2 3.2a1.5 1.5 0 0 0 1.4 1.8l5.6 4.5H4.9A8 8 0 0 1 6.4 9.5z\"/></svg>"
#define ICON_SAVE "<svg viewBox=\"0 0 24 24\"><path d=\"M17 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V7zm-5 16a3 3 0 1 1 0-6 3 3 0 0 1 0 6zm3-10H5V5h10z\"/></svg>"

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AirTrack</title><link rel=stylesheet href=/app.css></head><body>"
    "<aside class=side><div class=brand>" ICON_PLANE "AirTrack</div><nav>"
    "<a href=#dashboard class=active>" ICON_HOME "Dashboard</a>"
    "<a href=#location>" ICON_PIN "Location</a>"
    "<a href=#display>" ICON_MONITOR "Display</a>"
    "<a href=#storage>" ICON_SD "Storage</a>"
    "<a href=#system>" ICON_GEAR "System</a></nav></aside>"
    "<div class=page><header class=top>"
    "<span class=st><i class=dot></i><b class=ok>ONLINE</b></span>"
    "<span class=st>" ICON_WIFI "Wi-Fi <b id=ssid>";

static const char PAGE_AFTER_SSID[] =
    "</b></span><span class=st>" ICON_CLOUD "<b id=api class=\"";

static const char PAGE_AFTER_API[] =
    "</b></span><span class=st>" ICON_CLOCK "Updated <b id=upd>";

static const char PAGE_MAIN_START[] =
    "</b></span></header><main>";

static const char PAGE_FORM_START[] =
    "<form id=cfg method=post action=/api/v1/config autocomplete=off>"
    "<input type=hidden name=csrf_token value=\"";

static const char PAGE_GRID_START[] =
    "\"><div class=grid><div class=col>"
    "<section class=card id=dashboard><h2>Nearest aircraft</h2>";

/* SVG compass; the arrow/plane/arc are re-oriented by the script. */
static const char PAGE_COMPASS[] =
    "<div id=target class=nearest><svg class=compass viewBox=\"0 0 120 120\">"
    "<circle class=ring cx=60 cy=60 r=54 />"
    "<g class=tick><line x1=60 y1=6 x2=60 y2=14 class=major />"
    "<line x1=114 y1=60 x2=106 y2=60 class=major /><line x1=60 y1=114 x2=60 y2=106 class=major />"
    "<line x1=6 y1=60 x2=14 y2=60 class=major />"
    "<line x1=87 y1=13.2 x2=84.5 y2=17.6 /><line x1=106.8 y1=33 x2=102.4 y2=35.5 />"
    "<line x1=106.8 y1=87 x2=102.4 y2=84.5 /><line x1=87 y1=106.8 x2=84.5 y2=102.4 />"
    "<line x1=33 y1=106.8 x2=35.5 y2=102.4 /><line x1=13.2 y1=87 x2=17.6 y2=84.5 />"
    "<line x1=13.2 y1=33 x2=17.6 y2=35.5 /><line x1=33 y1=13.2 x2=35.5 y2=17.6 /></g>"
    "<text x=60 y=23>N</text><text x=97 y=60.5>E</text><text x=60 y=98>S</text><text x=23 y=60.5>W</text>";

static const char PAGE_COMPASS_TAIL[] =
    "<path d=\"M60 41l3 4 1 11 18 11v4l-18-6-1 10 6 5v3l-9-2-9 2v-3l6-5-1-10-18 6v-4l18-11 1-11z\"/></g>"
    "</svg><div class=who><div class=cs id=id>";

static const char PAGE_FACTS_END[] =
    "</ul></div></div>";

static const char PAGE_TABLE_HEAD[] =
    "<div class=tbl><table><thead><tr><th>Aircraft</th><th>Type</th><th>Route</th><th>Distance</th>"
    "<th>Bearing</th><th>Altitude</th><th>Speed</th><th>Squawk</th></tr></thead>"
    "<tbody id=rows>";

static const char PAGE_FOOTER[] =
    "<footer>Data provided by <a href=https://adsb.fi>adsb.fi</a> &middot; "
    "Personal, non-commercial use. Not for navigation or collision avoidance."
    "</footer></main></div><script src=/app.js></script></body></html>";

static SemaphoreHandle_t lifecycle_lock(void)
{
    taskENTER_CRITICAL(&s_lifecycle_lock_init);
    if (s_lifecycle_lock == NULL) {
        s_lifecycle_lock =
            xSemaphoreCreateMutexStatic(&s_lifecycle_lock_storage);
    }
    SemaphoreHandle_t lock = s_lifecycle_lock;
    taskEXIT_CRITICAL(&s_lifecycle_lock_init);
    return lock;
}

static bool valid_utf8(const unsigned char *text, size_t length)
{
    size_t index = 0U;
    while (index < length) {
        const unsigned char lead = text[index];
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }

        size_t continuation_count;
        unsigned char second_min = 0x80U;
        unsigned char second_max = 0xbfU;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation_count = 1U;
            if (lead == 0xc2U) {
                second_min = 0xa0U;
            }
        } else if (lead == 0xe0U) {
            continuation_count = 2U;
            second_min = 0xa0U;
        } else if (lead >= 0xe1U && lead <= 0xecU) {
            continuation_count = 2U;
        } else if (lead == 0xedU) {
            continuation_count = 2U;
            second_max = 0x9fU;
        } else if (lead >= 0xeeU && lead <= 0xefU) {
            continuation_count = 2U;
        } else if (lead == 0xf0U) {
            continuation_count = 3U;
            second_min = 0x90U;
        } else if (lead >= 0xf1U && lead <= 0xf3U) {
            continuation_count = 3U;
        } else if (lead == 0xf4U) {
            continuation_count = 3U;
            second_max = 0x8fU;
        } else {
            return false;
        }

        if (continuation_count >= length - index) {
            return false;
        }
        const unsigned char second = text[index + 1U];
        if (second < second_min || second > second_max) {
            return false;
        }
        for (size_t offset = 2U; offset <= continuation_count; ++offset) {
            const unsigned char continuation = text[index + offset];
            if (continuation < 0x80U || continuation > 0xbfU) {
                return false;
            }
        }
        index += continuation_count + 1U;
    }
    return true;
}

static bool valid_display_text(const char *value, size_t max_length)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, max_length + 1U);
    if (length == 0U || length > max_length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (character < 0x20U || character == 0x7fU) {
            return false;
        }
    }
    return valid_utf8((const unsigned char *)value, length);
}

static bool valid_ipv4_address(const char *address)
{
    if (address == NULL) {
        return false;
    }
    const size_t length =
        strnlen(address, STATUS_WEB_IPV4_TEXT_MAX_BYTES + 1U);
    if (length < sizeof("0.0.0.0") - 1U ||
        length > STATUS_WEB_IPV4_TEXT_MAX_BYTES) {
        return false;
    }

    const char *cursor = address;
    for (size_t part = 0U; part < 4U; ++part) {
        unsigned value = 0U;
        size_t digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = (value * 10U) + (unsigned)(*cursor - '0');
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

static void copy_text(char *destination, size_t capacity, const char *source)
{
    const size_t length = strnlen(source, capacity - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static esp_err_t normalize_snapshot(
    const status_web_snapshot_t *source,
    status_web_snapshot_storage_t *destination)
{
    if (source == NULL || destination == NULL || source->settings == NULL ||
        source->aircraft == NULL ||
        !valid_display_text(source->ssid, STATUS_WEB_SSID_MAX_BYTES) ||
        !valid_ipv4_address(source->ip_address) ||
        airtrack_settings_validate(source->settings) != ESP_OK ||
        source->aircraft->aircraft_count > AIRTRACK_MAX_AIRCRAFT) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(destination, 0, sizeof(*destination));
    copy_text(destination->ssid, sizeof(destination->ssid), source->ssid);
    copy_text(destination->ip_address, sizeof(destination->ip_address),
              source->ip_address);
    destination->rssi_available = source->rssi_available;
    destination->rssi_dbm = source->rssi_dbm;
    destination->sd_mounted = source->sd_mounted;
    destination->sd_logging_enabled = source->sd_logging_enabled;
    destination->sd_records_written = source->sd_records_written;
    destination->sd_log_bytes = source->sd_log_bytes;
    destination->sd_log_files = source->sd_log_files;
    destination->sd_files_pruned = source->sd_files_pruned;
    destination->flash_bytes = source->flash_bytes;
    destination->uptime_s = source->uptime_s;
    destination->free_heap_bytes = source->free_heap_bytes;
    destination->minimum_free_heap_bytes = source->minimum_free_heap_bytes;
    destination->time_synchronized = source->time_synchronized;
    destination->night = source->night;
    destination->local_minutes = source->local_minutes;
    destination->polls_ok = source->polls_ok;
    destination->polls_failed = source->polls_failed;
    destination->tls_connections = source->tls_connections;
    destination->settings = *source->settings;
    destination->aircraft = *source->aircraft;
    return ESP_OK;
}

static void replace_snapshot(const status_web_snapshot_storage_t *snapshot)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = *snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static status_web_snapshot_storage_t copy_snapshot(void)
{
    status_web_snapshot_storage_t snapshot;
    taskENTER_CRITICAL(&s_snapshot_lock);
    snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return snapshot;
}

static void clear_snapshot(void)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static void copy_firmware_version(
    char version[STATUS_WEB_VERSION_MAX_BYTES + 1U])
{
    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL) {
        memcpy(version, "unknown", sizeof("unknown"));
        return;
    }

    const size_t length =
        strnlen(description->version, sizeof(description->version));
    memcpy(version, description->version, length);
    version[length] = '\0';
}

static esp_err_t set_security_headers(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    }
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(
            request, "Content-Security-Policy",
            "default-src 'none'; style-src 'self'; script-src 'self'; "
            "connect-src 'self'; form-action 'self'; base-uri 'none'; "
            "frame-ancestors 'none'");
    }
    if (result == ESP_OK) {
        result =
            httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    }
    return result;
}

static esp_err_t send_plain_error(httpd_req_t *request, const char *status,
                                  const char *message)
{
    esp_err_t result = httpd_resp_set_status(request, status);
    if (result == ESP_OK) {
        result = httpd_resp_set_type(request, "text/plain; charset=utf-8");
    }
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = httpd_resp_sendstr(request, message);
    }
    return result;
}

static bool request_has_canonical_host(
    httpd_req_t *request, const status_web_snapshot_storage_t *snapshot)
{
    const size_t address_length = strlen(snapshot->ip_address);
    const size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    if (host_length != address_length && host_length != address_length + 3U) {
        return false;
    }

    char host[STATUS_WEB_HOST_MAX_BYTES];
    if (host_length + 1U > sizeof(host) ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) !=
            ESP_OK ||
        memcmp(host, snapshot->ip_address, address_length) != 0) {
        return false;
    }
    return host_length == address_length ||
           strcmp(host + address_length, ":80") == 0;
}

static esp_err_t send_canonical_redirect(
    httpd_req_t *request, const status_web_snapshot_storage_t *snapshot,
    const char *path)
{
    char location[STATUS_WEB_LOCATION_MAX_BYTES];
    const int length = snprintf(location, sizeof(location), "http://%s%s",
                                snapshot->ip_address, path);
    if (length < 0 || (size_t)length >= sizeof(location)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result = httpd_resp_set_status(request, "302 Found");
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request, "Location", location);
    }
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send(request, NULL, 0U);
    }
    return result;
}

static esp_err_t send_html_chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t flush_escape_buffer(httpd_req_t *request, char *buffer,
                                     size_t *used)
{
    if (*used == 0U) {
        return ESP_OK;
    }
    const esp_err_t result = httpd_resp_send_chunk(request, buffer, *used);
    *used = 0U;
    return result;
}

static esp_err_t send_html_escaped(httpd_req_t *request, const char *text)
{
    char buffer[96];
    size_t used = 0U;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        const char *replacement = NULL;
        switch (*cursor) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&#39;";
            break;
        default:
            break;
        }

        const size_t replacement_length =
            replacement == NULL ? 1U : strlen(replacement);
        if (used + replacement_length > sizeof(buffer)) {
            const esp_err_t result =
                flush_escape_buffer(request, buffer, &used);
            if (result != ESP_OK) {
                return result;
            }
        }
        if (replacement == NULL) {
            buffer[used++] = (char)*cursor;
        } else {
            memcpy(buffer + used, replacement, replacement_length);
            used += replacement_length;
        }
    }
    return flush_escape_buffer(request, buffer, &used);
}

static esp_err_t send_json_escaped(httpd_req_t *request, const char *text)
{
    char buffer[128];
    size_t used = 0U;
    static const char hex[] = "0123456789abcdef";

    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        char replacement[6];
        const char *encoded = replacement;
        size_t encoded_length;
        if (*cursor == '"' || *cursor == '\\') {
            replacement[0] = '\\';
            replacement[1] = (char)*cursor;
            encoded_length = 2U;
        } else if (*cursor < 0x20U) {
            replacement[0] = '\\';
            replacement[1] = 'u';
            replacement[2] = '0';
            replacement[3] = '0';
            replacement[4] = hex[*cursor >> 4U];
            replacement[5] = hex[*cursor & 0x0fU];
            encoded_length = sizeof(replacement);
        } else {
            encoded = (const char *)cursor;
            encoded_length = 1U;
        }

        if (used + encoded_length > sizeof(buffer)) {
            const esp_err_t result =
                flush_escape_buffer(request, buffer, &used);
            if (result != ESP_OK) {
                return result;
            }
        }
        memcpy(buffer + used, encoded, encoded_length);
        used += encoded_length;
    }
    return flush_escape_buffer(request, buffer, &used);
}

static const char *aircraft_identity(const airtrack_aircraft_t *aircraft)
{
    if (aircraft->callsign[0] != '\0') {
        return aircraft->callsign;
    }
    if (aircraft->registration[0] != '\0') {
        return aircraft->registration;
    }
    return aircraft->hex;
}

static float display_distance(float nautical_miles,
                              airtrack_distance_unit_t unit)
{
    if (unit == AIRTRACK_DISTANCE_KM) {
        return nautical_miles * 1.852f;
    }
    if (unit == AIRTRACK_DISTANCE_MI) {
        return nautical_miles * 1.150779f;
    }
    return nautical_miles;
}

static const char *distance_suffix(airtrack_distance_unit_t unit)
{
    return unit == AIRTRACK_DISTANCE_KM ? "km"
           : unit == AIRTRACK_DISTANCE_MI ? "mi" : "NM";
}

static double seconds_since_success(
    const status_web_snapshot_storage_t *snapshot)
{
    const int64_t current_ms = (int64_t)snapshot->uptime_s * 1000LL;
    if (snapshot->aircraft.last_success_monotonic_ms > 0 &&
        current_ms > snapshot->aircraft.last_success_monotonic_ms) {
        return (double)(current_ms -
                        snapshot->aircraft.last_success_monotonic_ms) / 1000.0;
    }
    return 0.0;
}

static double current_position_age_s(
    const status_web_snapshot_storage_t *snapshot,
    const airtrack_aircraft_t *aircraft)
{
    return aircraft->seen_pos_s + seconds_since_success(snapshot);
}

static esp_err_t send_aircraft_row(httpd_req_t *request,
                                   const status_web_snapshot_storage_t *snapshot,
                                   const airtrack_aircraft_t *aircraft)
{
    esp_err_t result = send_html_chunk(
        request, aircraft->emergency ? "<tr class=emergency><td>" : "<tr><td>");
    if (result == ESP_OK) {
        result = send_html_escaped(request, aircraft_identity(aircraft));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</td><td>");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, aircraft->aircraft_type);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</td><td>");
    }
    if (result == ESP_OK && aircraft->route_valid) {
        char route[16];
        (void)snprintf(route, sizeof(route), "%s&rarr;%s", aircraft->route_from,
                       aircraft->route_to);
        result = send_html_chunk(request, route);
    }
    if (result == ESP_OK) {
        char cells[160];
        char altitude[24];
        if (aircraft->ground) {
            memcpy(altitude, "on the ground", sizeof("on the ground"));
        } else if (aircraft->altitude_valid) {
            (void)snprintf(altitude, sizeof(altitude), "%ld ft",
                           (long)aircraft->altitude_ft);
        } else {
            memcpy(altitude, "--", sizeof("--"));
        }
        char speed[16];
        if (aircraft->ground_speed_valid) {
            (void)snprintf(speed, sizeof(speed), "%.0f kt",
                           (double)aircraft->ground_speed_kt);
        } else {
            memcpy(speed, "--", sizeof("--"));
        }
        const int length = snprintf(
            cells, sizeof(cells),
            "</td><td>%.1f %s</td><td>%03.0f&deg;</td><td>%s</td><td>%s</td><td>",
            (double)display_distance(aircraft->distance_nm,
                                     snapshot->settings.distance_unit),
            distance_suffix(snapshot->settings.distance_unit),
            (double)aircraft->bearing_deg, altitude, speed);
        result = (length < 0 || (size_t)length >= sizeof(cells))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, cells, (size_t)length);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, aircraft->squawk);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</td></tr>");
    }
    return result;
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
    return names[(unsigned)((normalized + 11.25f) / 22.5f) % 16U];
}

static const char *api_class(airtrack_feed_state_t state)
{
    switch (state) {
    case AIRTRACK_FEED_LIVE:
    case AIRTRACK_FEED_EMPTY:
        return "ok";
    case AIRTRACK_FEED_OFFLINE:
        return "bad";
    case AIRTRACK_FEED_SEARCHING:
        return "";
    default:
        return "warn";
    }
}

static const char *api_text(airtrack_feed_state_t state)
{
    switch (state) {
    case AIRTRACK_FEED_LIVE:
    case AIRTRACK_FEED_EMPTY:
        return "API OK";
    case AIRTRACK_FEED_STALE:
        return "API STALE";
    case AIRTRACK_FEED_OFFLINE:
        return "API OFFLINE";
    case AIRTRACK_FEED_TIME_SYNC:
        return "TIME SYNC";
    case AIRTRACK_FEED_CONFIG_REQUIRED:
        return "SET LOCATION";
    default:
        return "API ...";
    }
}

/* Distance to the known destination (NM) and ETA seconds (-1 if unknown). */
static long route_remaining(const airtrack_aircraft_t *aircraft, float *remaining_nm)
{
    *remaining_nm = -1.0f;
    if (!aircraft->destination_valid) {
        return -1;
    }
    float bearing;
    airtrack_geometry(aircraft->latitude, aircraft->longitude,
                      aircraft->destination_latitude,
                      aircraft->destination_longitude, remaining_nm, &bearing);
    if (aircraft->ground_speed_valid && aircraft->ground_speed_kt >= 60.0f) {
        return (long)(*remaining_nm / aircraft->ground_speed_kt * 3600.0f);
    }
    return -1;
}

static esp_err_t send_chunk_or_size(httpd_req_t *request, const char *text,
                                    int length, size_t capacity)
{
    if (length < 0 || (size_t)length >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_send_chunk(request, text, (size_t)length);
}

static esp_err_t send_nearest_card(httpd_req_t *request,
                                   const status_web_snapshot_storage_t *snapshot)
{
    const bool have_target = snapshot->aircraft.aircraft_count > 0U;
    const airtrack_aircraft_t *aircraft = &snapshot->aircraft.aircraft[0];
    esp_err_t result = ESP_OK;
    char text[1024];

    if (!snapshot->settings.location_configured) {
        result = send_html_chunk(request,
            "<div class=banner>Enter this display's fixed location under "
            "<b>Location</b> and save to start tracking.</div>");
    } else if (snapshot->settings.latitude_e7 == AIRTRACK_PLACEHOLDER_LATITUDE_E7 &&
               snapshot->settings.longitude_e7 == AIRTRACK_PLACEHOLDER_LONGITUDE_E7) {
        result = send_html_chunk(request,
            "<div class=banner>Tracking around <b>Seattle&ndash;Tacoma International</b> "
            "(the setup placeholder). Enter this display's real position under "
            "<b>Location</b> &mdash; or use the location button &mdash; and save.</div>");
    } else if (snapshot->settings.focus_flight[0] != '\0') {
        result = send_html_chunk(request,
            "<div class=banner>Following <b>");
        if (result == ESP_OK) {
            result = send_html_escaped(request, snapshot->settings.focus_flight);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request,
                "</b> only. Clear <b>Track a single flight</b> under Location "
                "to return to the nearest aircraft.</div>");
        }
    }

    /* Empty-state block. */
    if (result == ESP_OK) {
        const int length = snprintf(
            text, sizeof(text),
            "<div id=empty%s><p id=ehead class=big-empty>%s</p><p id=esub class=sub>"
            "within %u NM &middot; feed %s%s%s</p></div>",
            have_target ? " hidden" : "",
            snapshot->aircraft.state == AIRTRACK_FEED_EMPTY ? "No recent reports"
            : snapshot->aircraft.state == AIRTRACK_FEED_CONFIG_REQUIRED
                ? "Set the tracking location to start"
                : "Waiting for aircraft data",
            (unsigned)snapshot->settings.radius_nm,
            airtrack_feed_state_name(snapshot->aircraft.state),
            snapshot->aircraft.error != AIRTRACK_ERROR_NONE ? " &middot; " : "",
            snapshot->aircraft.error != AIRTRACK_ERROR_NONE
                ? airtrack_feed_error_name(snapshot->aircraft.error) : "");
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }

    /* Compass + facts (kept in the DOM while empty so the script can reveal it). */
    if (result == ESP_OK && !have_target) {
        result = send_html_chunk(request, "<div hidden>");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_COMPASS);
    }
    if (result == ESP_OK) {
        const float bearing = have_target ? aircraft->bearing_deg : 0.0f;
        const float track = have_target && aircraft->track_valid
                                ? aircraft->track_deg : bearing;
        const double s0 = (double)(bearing - 14.0f) * 3.14159265 / 180.0;
        const double s1 = (double)(bearing + 14.0f) * 3.14159265 / 180.0;
        const int length = snprintf(
            text, sizeof(text),
            "<path id=arc class=arc%s d=\"M%.1f,%.1f A54,54 0 0 1 %.1f,%.1f\"/>"
            "<g id=arrow transform=\"rotate(%.1f 60 60)\"><line x1=60 y1=42 x2=60 y2=14 />"
            "<polyline points=\"52,22 60,14 68,22\" /></g>"
            "<g id=plane transform=\"rotate(%.1f 60 60)\">",
            have_target ? "" : " hidden",
            60.0 + 54.0 * sin(s0), 60.0 - 54.0 * cos(s0),
            60.0 + 54.0 * sin(s1), 60.0 - 54.0 * cos(s1),
            (double)bearing, (double)track);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_COMPASS_TAIL);
    }
    if (result == ESP_OK && have_target) {
        result = send_html_escaped(request, aircraft_identity(aircraft));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</div><div class=meta id=meta>");
    }
    if (result == ESP_OK && have_target) {
        if (aircraft->aircraft_type[0] != '\0') {
            result = send_html_escaped(request, aircraft->aircraft_type);
        }
        if (result == ESP_OK && aircraft->registration[0] != '\0' &&
            strcmp(aircraft->registration, aircraft_identity(aircraft)) != 0) {
            if (aircraft->aircraft_type[0] != '\0') {
                result = send_html_chunk(request, " &middot; ");
            }
            if (result == ESP_OK) {
                result = send_html_escaped(request, aircraft->registration);
            }
        }
        if (result == ESP_OK && aircraft->emergency) {
            result = send_html_chunk(request, " &middot; EMERGENCY ");
            if (result == ESP_OK) {
                result = send_html_escaped(request, aircraft->squawk);
            }
        }
    }
    if (result == ESP_OK) {
        char altitude[64];
        char speed[48];
        char route_text[96] = "";
        if (have_target && aircraft->route_valid) {
            float remaining_nm = -1.0f;
            long eta_s = route_remaining(aircraft, &remaining_nm);
            int used = snprintf(route_text, sizeof(route_text), "%s &rarr; %s",
                                aircraft->route_from, aircraft->route_to);
            if (used > 0 && remaining_nm >= 0.0f) {
                used += snprintf(route_text + used, sizeof(route_text) - (size_t)used,
                                 " &middot; %.0f %s to go",
                                 (double)display_distance(remaining_nm,
                                     snapshot->settings.distance_unit),
                                 distance_suffix(snapshot->settings.distance_unit));
            }
            if (used > 0 && eta_s >= 0) {
                (void)snprintf(route_text + used, sizeof(route_text) - (size_t)used,
                               " &middot; ETA %ld:%02ld", eta_s / 3600, (eta_s % 3600) / 60);
            }
        }
        if (!have_target) {
            memcpy(altitude, "--", sizeof("--"));
            memcpy(speed, "--", sizeof("--"));
        } else {
            char vertical[32] = "";
            if (aircraft->vertical_rate_valid && !aircraft->ground) {
                if (aircraft->vertical_rate_fpm == 0) {
                    memcpy(vertical, " &middot; level", sizeof(" &middot; level"));
                } else {
                    (void)snprintf(vertical, sizeof(vertical),
                                   " &middot; %s %ld fpm",
                                   aircraft->vertical_rate_fpm > 0 ? "&uarr;" : "&darr;",
                                   labs((long)aircraft->vertical_rate_fpm));
                }
            }
            if (aircraft->ground) {
                (void)snprintf(altitude, sizeof(altitude), "on the ground");
            } else if (aircraft->altitude_valid) {
                (void)snprintf(altitude, sizeof(altitude), "%ld ft%s",
                               (long)aircraft->altitude_ft, vertical);
            } else {
                (void)snprintf(altitude, sizeof(altitude), "--%s", vertical);
            }
            char track[24] = "";
            if (aircraft->track_valid) {
                (void)snprintf(track, sizeof(track), " &middot; trk %03.0f&deg;",
                               (double)aircraft->track_deg);
            }
            if (aircraft->ground_speed_valid) {
                (void)snprintf(speed, sizeof(speed), "%.0f kt%s",
                               (double)aircraft->ground_speed_kt, track);
            } else {
                (void)snprintf(speed, sizeof(speed), "--%s", track);
            }
        }
        const int length = snprintf(
            text, sizeof(text),
            "</div><ul class=facts><li>" ICON_NAV "<span id=brg>%.1f %s &middot; %s &middot; %03.0f&deg;</span></li>"
            "<li>" ICON_MTN "<span id=alt>%s</span></li>"
            "<li>" ICON_GAUGE "<span id=spd>%s</span></li>"
            "<li id=routerow%s>" ICON_PLANE "<span id=route>%s</span></li>",
            have_target ? (double)display_distance(
                aircraft->distance_nm, snapshot->settings.distance_unit) : 0.0,
            distance_suffix(snapshot->settings.distance_unit),
            have_target ? cardinal_name(aircraft->bearing_deg) : "--",
            have_target ? (double)aircraft->bearing_deg : 0.0,
            altitude, speed,
            have_target && aircraft->route_valid ? "" : " hidden",
            route_text);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FACTS_END);
    }
    if (result == ESP_OK && !have_target) {
        result = send_html_chunk(request, "</div>");
    }

    /* Top-five table. */
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_TABLE_HEAD);
    }
    for (size_t index = 0U;
         result == ESP_OK && index < snapshot->aircraft.aircraft_count; ++index) {
        result = send_aircraft_row(request, snapshot,
                                   &snapshot->aircraft.aircraft[index]);
    }
    if (result == ESP_OK) {
        const int length = snprintf(
            text, sizeof(text),
            "</tbody></table></div><p id=counts class=hint>%lu shown of %lu reports "
            "within %u NM</p></section>",
            (unsigned long)snapshot->aircraft.aircraft_accepted,
            (unsigned long)snapshot->aircraft.aircraft_reported,
            (unsigned)snapshot->settings.radius_nm);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    return result;
}

static esp_err_t send_option(httpd_req_t *request, const char *value,
                             const char *label, bool selected)
{
    esp_err_t result = send_html_chunk(request, "<option value=");
    if (result == ESP_OK) {
        result = send_html_chunk(request, value);
    }
    if (result == ESP_OK && selected) {
        result = send_html_chunk(request, " selected");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, ">");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, label);
    }
    return result == ESP_OK ? send_html_chunk(request, "</option>") : result;
}

static esp_err_t send_settings_cards(httpd_req_t *request,
                                     const status_web_snapshot_storage_t *snapshot)
{
    const airtrack_settings_t *settings = &snapshot->settings;
    char text[2048];

    /* Left column continues: display card. */
    int length = snprintf(
        text, sizeof(text),
        "<section class=card id=display><h2>Display brightness "
        "<span class=val id=brv>%u%%</span></h2>"
        "<input type=range id=br name=brightness min=0 max=50 value=%u>"
        "<div class=ticks><span>0%%</span><span>25%%</span><span>50%%</span></div>"
        "<p class=hint>50%% is the hardware maximum for this panel.</p>"
        "<h3>Distance units</h3><select name=units>",
        (unsigned)settings->brightness_percent,
        (unsigned)settings->brightness_percent);
    esp_err_t result = send_chunk_or_size(request, text, length, sizeof(text));
    if (result == ESP_OK) {
        result = send_option(request, "nm", "Nautical miles",
                             settings->distance_unit == AIRTRACK_DISTANCE_NM);
    }
    if (result == ESP_OK) {
        result = send_option(request, "km", "Kilometres",
                             settings->distance_unit == AIRTRACK_DISTANCE_KM);
    }
    if (result == ESP_OK) {
        result = send_option(request, "mi", "Statute miles",
                             settings->distance_unit == AIRTRACK_DISTANCE_MI);
    }
    if (result == ESP_OK) {
        char from[8];
        char to[8];
        (void)snprintf(from, sizeof(from), "%02u:%02u",
                       (unsigned)settings->night_start_min / 60U,
                       (unsigned)settings->night_start_min % 60U);
        (void)snprintf(to, sizeof(to), "%02u:%02u",
                       (unsigned)settings->night_end_min / 60U,
                       (unsigned)settings->night_end_min % 60U);
        length = snprintf(
            text, sizeof(text),
            "</select><div class=row style=\"margin-top:18px\"><div><h3 style=\"margin:0\">Night schedule</h3>"
            "<p class=sub id=nightsub>%s</p></div><label class=switch>"
            "<input type=checkbox name=night value=1%s><i></i></label></div>"
            "<div class=two><label class=field>From<input name=night_from type=time value=\"%s\" required></label>"
            "<label class=field>To<input name=night_to type=time value=\"%s\" required></label></div>"
            "<h3>Night brightness <span class=val id=nbrv>%u%%</span></h3>"
            "<input type=range id=nbr name=night_brightness min=0 max=50 value=%u>"
            "<label class=check style=\"margin-top:10px\"><input type=checkbox name=night_led value=1%s> "
            "Status LED off at night</label>"
            "<h3>Timezone</h3><select id=tzsel><option value=\"\">Custom / UTC</option></select>"
            "<input name=tz id=tz type=text maxlength=47 placeholder=\"POSIX rule, e.g. PST8PDT,M3.2.0,M11.1.0 (blank = UTC)\" value=\"%s\">"
            "<p class=hint id=tzhint>Used only for the night schedule and the local time shown here.</p>"
            "</section></div><div class=col>",
            snapshot->night ? "Active now &middot; panel dimmed" : "Inactive",
            settings->night_enabled ? " checked" : "", from, to,
            (unsigned)settings->night_brightness_percent,
            (unsigned)settings->night_brightness_percent,
            settings->night_led_off ? " checked" : "",
            settings->timezone);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }

    /* Right column: location, radius, filter, interval, storage, save. */
    if (result == ESP_OK) {
        char latitude[24] = "";
        char longitude[24] = "";
        if (settings->location_configured) {
            (void)snprintf(latitude, sizeof(latitude), "%.6f",
                           (double)settings->latitude_e7 / 10000000.0);
            (void)snprintf(longitude, sizeof(longitude), "%.6f",
                           (double)settings->longitude_e7 / 10000000.0);
        }
        length = snprintf(
            text, sizeof(text),
            "<section class=card id=location><h2>Search location</h2><div class=two>"
            "<label class=field>Latitude<input name=latitude type=number step=any "
            "min=-90 max=90 placeholder=37.62131 required value=\"%s\"></label>"
            "<label class=field>Longitude<input name=longitude type=number step=any "
            "min=-180 max=180 placeholder=-122.37896 required value=\"%s\"></label>"
            "</div><div class=geo><button type=button id=geo class=ghost>" ICON_PIN
            "Use my location</button><input id=paste type=text placeholder="
            "\"or paste \u201947.35, -121.98\u2019 / a maps link\"></div>"
            "<p class=hint id=geohint>Fixed decimal-degree position of this display; "
            "used only to request nearby traffic from adsb.fi. Browsers share "
            "location only over HTTPS, so the button hands you to a small HTTPS "
            "helper page that brings the coordinates back here; or paste them.</p>"
            "<h3>Track a single flight</h3>"
            "<input name=focus type=text maxlength=8 placeholder=\"e.g. UAL205, N37267 or A280A4 (blank = nearest)\" value=\"%s\">"
            "<p class=hint>When set, only that callsign, registration, or hex is "
            "shown, with its route, distance to go, and an ETA estimated from "
            "ground speed. Scheduled times need a paid flight-schedule API and are "
            "not available.</p></section>"
            "<section class=card><h2>Search radius</h2><div class=range-row>"
            "<input type=range id=rad min=1 max=250 value=%u>"
            "<input type=number id=radn name=radius min=1 max=250 value=%u required>"
            "<span>NM</span></div><div class=ticks><span>1</span><span>50</span>"
            "<span>100</span><span>150</span><span>200</span><span>250</span></div>"
            "</section>",
            latitude, longitude, settings->focus_flight,
            (unsigned)settings->radius_nm, (unsigned)settings->radius_nm);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    if (result == ESP_OK) {
        length = snprintf(
            text, sizeof(text),
            "<section class=card><div class=row><div><h2>Aircraft filter</h2>"
            "<p class=sub>Airborne only &middot; hide aircraft reporting on the ground</p>"
            "</div><label class=switch><input type=checkbox name=airborne value=1%s><i></i>"
            "</label></div></section>"
            "<section class=card><h2>Refresh interval</h2><select name=poll>",
            settings->include_ground ? "" : " checked");
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    static const struct {
        const char *value;
        const char *label;
        uint16_t seconds;
    } intervals[] = {
        {"2", "2 seconds", 2}, {"5", "5 seconds", 5}, {"10", "10 seconds", 10},
        {"15", "15 seconds", 15}, {"30", "30 seconds", 30}, {"60", "1 minute", 60},
        {"300", "5 minutes", 300},
    };
    bool matched = false;
    for (size_t index = 0U; result == ESP_OK &&
         index < sizeof(intervals) / sizeof(intervals[0]); ++index) {
        const bool selected = settings->poll_interval_s == intervals[index].seconds;
        matched = matched || selected;
        result = send_option(request, intervals[index].value,
                             intervals[index].label, selected);
    }
    if (result == ESP_OK && !matched) {
        char value[8];
        char label[24];
        (void)snprintf(value, sizeof(value), "%u", (unsigned)settings->poll_interval_s);
        (void)snprintf(label, sizeof(label), "%u seconds", (unsigned)settings->poll_interval_s);
        result = send_option(request, value, label, true);
    }
    if (result == ESP_OK) {
        const bool logging = settings->logging_mode != AIRTRACK_LOGGING_OFF;
        static const struct { uint16_t minutes; const char *label; } windows[] = {
            {30U, "30 minutes"}, {60U, "hour"}, {360U, "6 hours"}, {1440U, "day"},
        };
        char window_options[320] = "";
        size_t used = 0U;
        bool matched = false;
        for (size_t index = 0U; index < sizeof(windows) / sizeof(windows[0]); ++index) {
            const bool selected = settings->sighting_window_min == windows[index].minutes;
            matched = matched || selected;
            const int written = snprintf(window_options + used, sizeof(window_options) - used,
                "<option value=%u%s>%s</option>", (unsigned)windows[index].minutes,
                selected ? " selected" : "", windows[index].label);
            if (written < 0 || (size_t)written >= sizeof(window_options) - used) {
                break;
            }
            used += (size_t)written;
        }
        if (!matched) {
            (void)snprintf(window_options + used, sizeof(window_options) - used,
                           "<option value=%u selected>%u minutes</option>",
                           (unsigned)settings->sighting_window_min,
                           (unsigned)settings->sighting_window_min);
        }
        char log_usage[96];
        (void)snprintf(log_usage, sizeof(log_usage),
                       "Using %.1f MiB in %lu file%s of the %u MiB cap &middot; %lu pruned",
                       (double)snapshot->sd_log_bytes / (1024.0 * 1024.0),
                       (unsigned long)snapshot->sd_log_files,
                       snapshot->sd_log_files == 1U ? "" : "s",
                       (unsigned)settings->retention_mib,
                       (unsigned long)snapshot->sd_files_pruned);
        length = snprintf(
            text, sizeof(text),
            "</select><p class=hint>How often adsb.fi is polled. Their public "
            "limit is one request per second.</p></section>"
            "<section class=card id=storage><div class=row><div><h2>SD sighting log</h2>"
            "<p class=sub id=sdsub>%s</p></div><label class=switch>"
            "<input type=checkbox name=logging value=1%s><i></i></label></div>"
            "<p class=hint>Every distinct aircraft that enters the tracked set is "
            "written as NDJSON under /airtrack/logs on a FAT32 card, once per "
            "window below. Nothing is written when logging is off.</p>"
            "<label class=field>Log each aircraft at most once per"
            "<select name=log_window>%s</select></label>"
            "<div class=two><label class=field>Size cap (MiB)"
            "<input name=retention type=number min=8 max=4096 value=%u></label>"
            "<label class=field>Keep days<input type=number value=%u disabled></label></div>"
            "<p class=hint id=logusage>%s</p>"
            "<div id=logs class=logs><div class=logbar><b>Log files</b>"
            "<span><button type=button id=logclear class=\"ghost danger\">Clear log</button> "
            "<button type=button id=logrefresh class=ghost>Refresh</button></span></div>"
            "<div id=loglist class=loglist>Loading&hellip;</div>"
            "<div id=logview hidden><div class=logbar><b id=logname></b>"
            "<a id=logdl class=ghost download>Download</a></div>"
            "<div class=tbl><table><thead><tr><th>Time (UTC)</th><th>Event</th>"
            "<th>Flight</th><th>Reg</th><th>Type</th><th>Route</th><th>Dist</th>"
            "<th>Alt</th><th>Speed</th></tr></thead><tbody id=logrows></tbody></table>"
            "</div></div></div></section>"
            "<div class=\"card actions\"><span id=toast class=toast></span>"
            "<button type=submit>" ICON_SAVE "Save changes</button></div>"
            "</div></div></form>",
            !snapshot->sd_mounted ? "No SD card detected"
            : logging ? "Enabled" : "Disabled &middot; card ready",
            logging ? " checked" : "",
            window_options,
            (unsigned)settings->retention_mib,
            (unsigned)settings->retention_days,
            log_usage);
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    return result;
}

static esp_err_t send_system_card(httpd_req_t *request,
                                  const status_web_snapshot_storage_t *snapshot)
{
    char version[STATUS_WEB_VERSION_MAX_BYTES + 1U];
    copy_firmware_version(version);
    char text[768];
    char signal[24];
    if (snapshot->rssi_available) {
        (void)snprintf(signal, sizeof(signal), "%d dBm", (int)snapshot->rssi_dbm);
    } else {
        memcpy(signal, "unavailable", sizeof("unavailable"));
    }
    char local_time[48];
    if (snapshot->local_minutes >= 0) {
        (void)snprintf(local_time, sizeof(local_time), "%02d:%02d %s%s",
                       snapshot->local_minutes / 60, snapshot->local_minutes % 60,
                       snapshot->settings.timezone[0] != '\0' ? "" : "UTC",
                       snapshot->night ? " &middot; night mode" : "");
    } else {
        memcpy(local_time, "--:--", sizeof("--:--"));
    }
    char sd[48];
    if (!snapshot->sd_mounted) {
        memcpy(sd, "No card", sizeof("No card"));
    } else if (snapshot->sd_logging_enabled) {
        (void)snprintf(sd, sizeof(sd), "Logging &middot; %lu records",
                       (unsigned long)snapshot->sd_records_written);
    } else {
        memcpy(sd, "Card mounted &middot; logging off",
               sizeof("Card mounted &middot; logging off"));
    }
    ota_status_t ota;
    (void)ota_get_status(&ota);
    esp_err_t result = send_html_chunk(request,
        "<section class=card id=system><div class=sys><div><h2>System</h2>"
        "<div class=kv><span>Firmware</span><b><span id=fw>");
    if (result == ESP_OK) {
        result = send_html_escaped(request, version);
    }
    if (result == ESP_OK) {
        char partition[640];
        const int length = snprintf(partition, sizeof(partition),
            "</span> <small class=slot>slot %s%s</small></b>"
            "<span>Update</span><b id=otamsg>%s</b>"
            "<div class=otarow>"
            "<button type=button id=otacheck class=ghost>Check for updates</button>"
            "<button type=button id=otainstall hidden>Install</button>"
            "<div id=otabar hidden><div id=otafill></div></div></div>",
            ota.running_partition, ota.pending_verify ? " (verifying)" : "",
            ota.state == OTA_STATE_AVAILABLE ? "Update available"
            : ota.state == OTA_STATE_UP_TO_DATE ? "Up to date" : "Not checked yet");
        result = send_chunk_or_size(request, partition, length, sizeof(partition));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "<span>Address</span><b>");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, snapshot->ip_address);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, " &middot; http://");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, snapshot->settings.hostname);
    }
    if (result == ESP_OK) {
        const int length = snprintf(
            text, sizeof(text),
            ".local</b><span>Wi-Fi signal</span><b id=rssi>%s</b>"
            "<span>Uptime</span><b id=uptime>%luh %02lum</b>"
            "<span>Time</span><b id=time>%s</b>"
            "<span>Local time</span><b id=ltime>%s</b>"
            "<span>Feed polls</span><b id=polls>%lu ok &middot; %lu failed &middot; %lu TLS sessions</b>"
            "<span>Heap</span><b id=heap>%lu KiB free &middot; min %lu KiB</b>"
            "<span>SD card</span><b id=sd>%s</b>"
            "<span>Flash</span><b>%lu MiB</b></div></div>"
            "<form id=rb method=post action=/api/v1/reboot>"
            "<input type=hidden name=csrf_token value=\"",
            signal,
            (unsigned long)(snapshot->uptime_s / 3600U),
            (unsigned long)((snapshot->uptime_s % 3600U) / 60U),
            snapshot->time_synchronized ? "synchronized" : "not yet synchronized",
            local_time,
            (unsigned long)snapshot->polls_ok, (unsigned long)snapshot->polls_failed,
            (unsigned long)snapshot->tls_connections,
            (unsigned long)(snapshot->free_heap_bytes / 1024U),
            (unsigned long)(snapshot->minimum_free_heap_bytes / 1024U),
            sd, (unsigned long)(snapshot->flash_bytes / (1024U * 1024U)));
        result = send_chunk_or_size(request, text, length, sizeof(text));
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_csrf_token);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request,
            "\"><button class=ghost type=submit>Restart device</button> "
            "<button class=\"ghost danger\" type=button id=factory>Factory reset</button></form>"
            "</div><p class=hint>Wi-Fi credentials are changed only from the "
            "isolated setup hotspot: hold BOOT for five seconds. Factory reset "
            "erases the SD sighting log, Wi-Fi, location and every option, "
            "generates a new setup hotspot password, and restarts into setup mode.</p></section>");
    }
    return result;
}

static esp_err_t status_page_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/");
    }
    esp_err_t result = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_HEAD);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, snapshot.ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_SSID);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, api_class(snapshot.aircraft.state));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\">");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, api_text(snapshot.aircraft.state));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_API);
    }
    if (result == ESP_OK) {
        char age[24];
        if (snapshot.aircraft.last_success_monotonic_ms > 0) {
            (void)snprintf(age, sizeof(age), "%.0fs ago",
                           seconds_since_success(&snapshot));
        } else {
            memcpy(age, "never", sizeof("never"));
        }
        result = send_html_chunk(request, age);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_MAIN_START);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_START);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_csrf_token);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_GRID_START);
    }
    if (result == ESP_OK) {
        result = send_nearest_card(request, &snapshot);
    }
    if (result == ESP_OK) {
        result = send_settings_cards(request, &snapshot);
    }
    if (result == ESP_OK) {
        result = send_system_card(request, &snapshot);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FOOTER);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t send_embedded_asset(httpd_req_t *request, const char *path,
                                     const char *content_type,
                                     const char *start, const char *end)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, path);
    }
    esp_err_t result = httpd_resp_set_type(request, content_type);
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        /* EMBED_TXTFILES appends a terminating NUL that must not be sent. */
        result = httpd_resp_send(request, start, (size_t)(end - start) - 1U);
    }
    return result;
}

static esp_err_t app_js_handler(httpd_req_t *request)
{
    return send_embedded_asset(request, "/app.js",
                               "application/javascript; charset=utf-8",
                               app_js_start, app_js_end);
}

static esp_err_t app_css_handler(httpd_req_t *request)
{
    return send_embedded_asset(request, "/app.css", "text/css; charset=utf-8",
                               app_css_start, app_css_end);
}

static esp_err_t status_api_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/api/v1/status");
    }

    char version[STATUS_WEB_VERSION_MAX_BYTES + 1U];
    copy_firmware_version(version);

    esp_err_t result =
        httpd_resp_set_type(request, "application/json; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request,
                                 "{\"device\":\"AirTrack\",\"firmware\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, version);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(
            request, "\",\"mode\":\"station\",\"connected\":true,\"ssid\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, snapshot.ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\",\"ip_address\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, snapshot.ip_address);
    }
    if (result == ESP_OK) {
        char rssi[8];
        int rssi_length;
        if (snapshot.rssi_available) {
            rssi_length = snprintf(rssi, sizeof(rssi), "%d",
                                   (int)snapshot.rssi_dbm);
        } else {
            memcpy(rssi, "null", sizeof("null"));
            rssi_length = (int)(sizeof("null") - 1U);
        }
        if (rssi_length < 0 || (size_t)rssi_length >= sizeof(rssi)) {
            return ESP_ERR_INVALID_SIZE;
        }

        char tail[640];
        const int length = snprintf(
            tail, sizeof(tail),
            "\",\"rssi_dbm\":%s,\"sd_mounted\":%s,\"flash_bytes\":%lu,"
            "\"uptime_s\":%lu,\"free_heap_bytes\":%lu,"
            "\"minimum_free_heap_bytes\":%lu,\"time_synchronized\":%s,"
            "\"feed_state\":\"%s\",\"feed_error\":\"%s\","
            "\"aircraft_count\":%u,\"http_status\":%d,"
            "\"polls_ok\":%lu,\"polls_failed\":%lu,\"tls_connections\":%lu,"
            "\"sd_logging\":%s,\"sd_records\":%lu,\"sd_log_bytes\":%llu,"
            "\"sd_log_files\":%lu,\"sd_files_pruned\":%lu,"
            "\"night\":%s,\"local_minutes\":%d}",
            rssi,
            snapshot.sd_mounted ? "true" : "false",
            (unsigned long)snapshot.flash_bytes,
            (unsigned long)snapshot.uptime_s,
            (unsigned long)snapshot.free_heap_bytes,
            (unsigned long)snapshot.minimum_free_heap_bytes,
            snapshot.time_synchronized ? "true" : "false",
            airtrack_feed_state_name(snapshot.aircraft.state),
            airtrack_feed_error_name(snapshot.aircraft.error),
            (unsigned)snapshot.aircraft.aircraft_count,
            snapshot.aircraft.http_status,
            (unsigned long)snapshot.polls_ok,
            (unsigned long)snapshot.polls_failed,
            (unsigned long)snapshot.tls_connections,
            snapshot.sd_logging_enabled ? "true" : "false",
            (unsigned long)snapshot.sd_records_written,
            (unsigned long long)snapshot.sd_log_bytes,
            (unsigned long)snapshot.sd_log_files,
            (unsigned long)snapshot.sd_files_pruned,
            snapshot.night ? "true" : "false", snapshot.local_minutes);
        if (length < 0 || (size_t)length >= sizeof(tail)) {
            return ESP_ERR_INVALID_SIZE;
        }
        result = send_html_chunk(request, tail);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t config_api_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/api/v1/config");
    }
    const airtrack_settings_t *settings = &snapshot.settings;
    char body[768];
    const int length = snprintf(
        body, sizeof(body),
        "{\"generation\":%llu,\"location_configured\":%s,"
        "\"latitude\":%.7f,\"longitude\":%.7f,\"radius_nm\":%u,"
        "\"poll_interval_s\":%u,\"max_position_age_s\":%u,"
        "\"include_ground\":%s,\"distance_unit\":\"%s\","
        "\"brightness_percent\":%u,\"logging_mode\":%u,\"retention_mib\":%u,"
        "\"sighting_window_min\":%u,"
        "\"retention_days\":%u,\"focus\":\"%s\",\"night_enabled\":%s,"
        "\"night_from\":\"%02u:%02u\",\"night_to\":\"%02u:%02u\","
        "\"night_brightness_percent\":%u,\"night_led_off\":%s,"
        "\"timezone\":\"%s\"}",
        (unsigned long long)settings->generation,
        settings->location_configured ? "true" : "false",
        (double)settings->latitude_e7 / 10000000.0,
        (double)settings->longitude_e7 / 10000000.0,
        (unsigned)settings->radius_nm,
        (unsigned)settings->poll_interval_s,
        (unsigned)settings->max_position_age_s,
        settings->include_ground ? "true" : "false",
        distance_suffix(settings->distance_unit),
        (unsigned)settings->brightness_percent,
        (unsigned)settings->logging_mode,
        (unsigned)settings->retention_mib,
        (unsigned)settings->sighting_window_min,
        (unsigned)settings->retention_days,
        settings->focus_flight,
        settings->night_enabled ? "true" : "false",
        (unsigned)settings->night_start_min / 60U, (unsigned)settings->night_start_min % 60U,
        (unsigned)settings->night_end_min / 60U, (unsigned)settings->night_end_min % 60U,
        (unsigned)settings->night_brightness_percent,
        settings->night_led_off ? "true" : "false",
        settings->timezone);
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t result = httpd_resp_set_type(request,
                                            "application/json; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send(request, body, (size_t)length);
    }
    return result;
}

static int form_hex(char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static esp_err_t decode_form(const char *input, size_t length,
                             char *output, size_t capacity)
{
    size_t used = 0U;
    for (size_t index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)input[index];
        if (byte == '+') {
            byte = ' ';
        } else if (byte == '%') {
            if (index + 2U >= length) {
                return ESP_ERR_INVALID_ARG;
            }
            const int high = form_hex(input[index + 1U]);
            const int low = form_hex(input[index + 2U]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            byte = (unsigned char)((high << 4) | low);
            index += 2U;
        }
        if (byte < 0x20U || byte == 0x7fU || used + 1U >= capacity) {
            return ESP_ERR_INVALID_ARG;
        }
        output[used++] = (char)byte;
    }
    output[used] = '\0';
    return ESP_OK;
}

static bool form_content_type(httpd_req_t *request)
{
    const size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (length == 0U || length > 63U) {
        return false;
    }
    char value[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", value,
                                    sizeof(value)) != ESP_OK) {
        return false;
    }
    char *parameters = strchr(value, ';');
    if (parameters != NULL) {
        *parameters = '\0';
    }
    char *start = value;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return strcasecmp(start, "application/x-www-form-urlencoded") == 0;
}

/* "HH:MM" -> minutes of day. */
static bool parse_clock(const char *text, unsigned *minutes)
{
    if (text == NULL || strlen(text) != 5U || text[2] != ':' ||
        !isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
        !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) {
        return false;
    }
    const unsigned hours = (unsigned)(text[0] - '0') * 10U + (unsigned)(text[1] - '0');
    const unsigned mins = (unsigned)(text[3] - '0') * 10U + (unsigned)(text[4] - '0');
    if (hours > 23U || mins > 59U) {
        return false;
    }
    *minutes = hours * 60U + mins;
    return true;
}

static bool parse_bounded_ulong(const char *text, unsigned long minimum,
                                unsigned long maximum, unsigned long *out)
{
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum ||
        value > maximum) {
        return false;
    }
    *out = value;
    return true;
}

typedef struct {
    bool have_csrf;
    bool csrf_ok;
    bool have_latitude;
    bool have_longitude;
    bool have_radius;
    bool have_poll;
    bool have_brightness;
    bool have_units;
    bool have_focus;
    bool have_retention;
    bool have_log_window;
    bool have_confirm;
    bool have_version;
    bool have_night_from;
    bool have_night_to;
    bool have_night_brightness;
    bool have_tz;
    bool airborne;
    bool logging;
    bool night;
    bool night_led;
    char latitude[24];
    char longitude[24];
    char radius[8];
    char poll[8];
    char brightness[8];
    char units[4];
    char focus[AIRTRACK_FOCUS_MAX_LENGTH + 1U];
    char retention[8];
    char log_window[8];
    char confirm[8];
    char version[OTA_VERSION_MAX_BYTES + 1U];
    char night_from[8];
    char night_to[8];
    char night_brightness[8];
    char tz[AIRTRACK_TZ_MAX_LENGTH + 1U];
} settings_form_t;

/*
 * Decode a form body into fixed fields.  Unknown keys and duplicates are
 * rejected outright; the caller decides which keys are required.
 */
static esp_err_t decode_settings_form(const char *body, size_t length,
                                      settings_form_t *form)
{
    memset(form, 0, sizeof(*form));
    char csrf[STATUS_WEB_CSRF_BYTES + 1U] = {0};
    char flag[4];
    size_t offset = 0U;
    while (offset < length) {
        const char *pair = body + offset;
        const char *separator = memchr(pair, '&', length - offset);
        const size_t pair_length = separator != NULL
                                       ? (size_t)(separator - pair)
                                       : length - offset;
        const char *equals = memchr(pair, '=', pair_length);
        if (pair_length == 0U || equals == NULL || equals == pair) {
            return ESP_ERR_INVALID_ARG;
        }
        char key[20];
        esp_err_t result = decode_form(pair, (size_t)(equals - pair), key,
                                       sizeof(key));
        if (result != ESP_OK) {
            return result;
        }
        const char *encoded = equals + 1;
        const size_t encoded_length = pair_length - (size_t)(encoded - pair);
#define FIELD(name, buffer, flagvar)                                          \
        else if (strcmp(key, name) == 0 && !form->flagvar) {                  \
            result = decode_form(encoded, encoded_length, buffer,             \
                                 sizeof(buffer));                             \
            form->flagvar = result == ESP_OK;                                 \
        }
        if (false) {
        }
        FIELD("csrf_token", csrf, have_csrf)
        FIELD("latitude", form->latitude, have_latitude)
        FIELD("longitude", form->longitude, have_longitude)
        FIELD("radius", form->radius, have_radius)
        FIELD("poll", form->poll, have_poll)
        FIELD("brightness", form->brightness, have_brightness)
        FIELD("units", form->units, have_units)
        FIELD("focus", form->focus, have_focus)
        FIELD("retention", form->retention, have_retention)
        FIELD("log_window", form->log_window, have_log_window)
        FIELD("confirm", form->confirm, have_confirm)
        FIELD("version", form->version, have_version)
        FIELD("night_from", form->night_from, have_night_from)
        FIELD("night_to", form->night_to, have_night_to)
        FIELD("night_brightness", form->night_brightness, have_night_brightness)
        FIELD("tz", form->tz, have_tz)
#undef FIELD
        else if (strcmp(key, "airborne") == 0 && !form->airborne) {
            result = decode_form(encoded, encoded_length, flag, sizeof(flag));
            form->airborne = result == ESP_OK && strcmp(flag, "1") == 0;
        } else if (strcmp(key, "logging") == 0 && !form->logging) {
            result = decode_form(encoded, encoded_length, flag, sizeof(flag));
            form->logging = result == ESP_OK && strcmp(flag, "1") == 0;
        } else if (strcmp(key, "night") == 0 && !form->night) {
            result = decode_form(encoded, encoded_length, flag, sizeof(flag));
            form->night = result == ESP_OK && strcmp(flag, "1") == 0;
        } else if (strcmp(key, "night_led") == 0 && !form->night_led) {
            result = decode_form(encoded, encoded_length, flag, sizeof(flag));
            form->night_led = result == ESP_OK && strcmp(flag, "1") == 0;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        if (result != ESP_OK) {
            return result;
        }
        offset += pair_length + (separator != NULL ? 1U : 0U);
    }
    if (form->have_csrf && strlen(csrf) == STATUS_WEB_CSRF_BYTES) {
        uint8_t difference = 0U;
        for (size_t index = 0U; index < STATUS_WEB_CSRF_BYTES; ++index) {
            difference |= (uint8_t)csrf[index] ^ (uint8_t)s_csrf_token[index];
        }
        form->csrf_ok = difference == 0U;
    }
    return ESP_OK;
}

static esp_err_t apply_settings_form(const settings_form_t *form,
                                     airtrack_settings_t *settings)
{
    if (!form->have_latitude || !form->have_longitude || !form->have_radius) {
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    char *latitude_end = NULL;
    char *longitude_end = NULL;
    const double latitude = strtod(form->latitude, &latitude_end);
    const double longitude = strtod(form->longitude, &longitude_end);
    unsigned long numeric = 0UL;
    if (errno != 0 || latitude_end == form->latitude || *latitude_end != '\0' ||
        longitude_end == form->longitude || *longitude_end != '\0' ||
        !isfinite(latitude) || !isfinite(longitude) ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0 ||
        !parse_bounded_ulong(form->radius, 1UL, 250UL, &numeric)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->location_configured = true;
    settings->latitude_e7 = (int32_t)llround(latitude * 10000000.0);
    settings->longitude_e7 = (int32_t)llround(longitude * 10000000.0);
    settings->radius_nm = (uint16_t)numeric;
    if (form->have_poll) {
        if (!parse_bounded_ulong(form->poll, 2UL, 300UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->poll_interval_s = (uint16_t)numeric;
    }
    if (form->have_brightness) {
        if (!parse_bounded_ulong(form->brightness, 0UL, 50UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->brightness_percent = (uint8_t)numeric;
    }
    if (form->have_units) {
        if (strcmp(form->units, "nm") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_NM;
        } else if (strcmp(form->units, "km") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_KM;
        } else if (strcmp(form->units, "mi") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_MI;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (form->have_focus) {
        /* Upper-case, drop spaces; the validator rejects anything else. */
        size_t used = 0U;
        for (const char *cursor = form->focus; *cursor != '\0'; ++cursor) {
            if (*cursor == ' ') {
                continue;
            }
            if (used >= AIRTRACK_FOCUS_MAX_LENGTH) {
                return ESP_ERR_INVALID_ARG;
            }
            settings->focus_flight[used++] =
                (char)toupper((unsigned char)*cursor);
        }
        settings->focus_flight[used] = '\0';
    }
    if (form->have_retention) {
        if (!parse_bounded_ulong(form->retention, 8UL, 4096UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->retention_mib = (uint16_t)numeric;
    }
    if (form->have_log_window) {
        if (!parse_bounded_ulong(form->log_window, 5UL, 1440UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->sighting_window_min = (uint16_t)numeric;
    }
    if (form->have_night_from || form->have_night_to || form->have_night_brightness) {
        /* The night section is posted as a whole; checkboxes absent = off. */
        unsigned start = 0U;
        unsigned end = 0U;
        if (!parse_clock(form->night_from, &start) || !parse_clock(form->night_to, &end) ||
            !parse_bounded_ulong(form->night_brightness, 0UL, 50UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->night_enabled = form->night;
        settings->night_start_min = (uint16_t)start;
        settings->night_end_min = (uint16_t)end;
        settings->night_brightness_percent = (uint8_t)numeric;
        settings->night_led_off = form->night_led;
    }
    if (form->have_tz) {
        (void)snprintf(settings->timezone, sizeof(settings->timezone), "%s", form->tz);
    }
    /* Checkboxes are absent when unchecked; the dashboard always posts the
     * complete form, so absence is an explicit "off". */
    settings->include_ground = !form->airborne;
    if (form->logging) {
        if (settings->logging_mode == AIRTRACK_LOGGING_OFF) {
            settings->logging_mode = AIRTRACK_LOGGING_TARGET_CHANGES;
        }
    } else {
        settings->logging_mode = AIRTRACK_LOGGING_OFF;
    }
    return airtrack_settings_validate(settings);
}

static bool wants_json(httpd_req_t *request)
{
    char accept[96];
    if (httpd_req_get_hdr_value_str(request, "Accept", accept,
                                    sizeof(accept)) != ESP_OK) {
        return false;
    }
    return strstr(accept, "application/json") != NULL;
}

static esp_err_t send_form_result(httpd_req_t *request, bool ok,
                                  const char *status, const char *message)
{
    if (wants_json(request)) {
        char body[160];
        const int length = snprintf(body, sizeof(body),
                                    "{\"ok\":%s,\"error\":\"%s\"}",
                                    ok ? "true" : "false", ok ? "" : message);
        esp_err_t result = httpd_resp_set_status(request, status);
        if (result == ESP_OK) {
            result = httpd_resp_set_type(request, "application/json; charset=utf-8");
        }
        if (result == ESP_OK) {
            result = set_security_headers(request);
        }
        if (result == ESP_OK) {
            result = send_chunk_or_size(request, body, length, sizeof(body));
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request, NULL, 0U);
        }
        return result;
    }
    if (ok) {
        const status_web_snapshot_storage_t snapshot = copy_snapshot();
        return send_canonical_redirect(request, &snapshot, "/");
    }
    return send_plain_error(request, status, message);
}

static esp_err_t read_form_body(httpd_req_t *request, char *body,
                                size_t capacity, size_t *received)
{
    *received = 0U;
    if (!form_content_type(request) || request->content_len <= 0 ||
        (size_t)request->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (*received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + *received,
            (size_t)request->content_len - *received);
        if (count <= 0) {
            memset(body, 0, capacity);
            return ESP_ERR_TIMEOUT;
        }
        *received += (size_t)count;
    }
    body[*received] = '\0';
    return ESP_OK;
}

static esp_err_t settings_form_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    if (s_save_settings == NULL) {
        return send_form_result(request, false, "503 Service Unavailable",
                                "Settings cannot be changed right now.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    if (result != ESP_OK) {
        return send_form_result(request, false, "400 Bad Request",
                                "Invalid or incomplete form.");
    }
    settings_form_t form;
    result = decode_settings_form(body, received, &form);
    memset(body, 0, sizeof(body));
    if (result != ESP_OK) {
        return send_form_result(request, false, "400 Bad Request",
                                "Unrecognised form fields.");
    }
    if (!form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    airtrack_settings_t settings = snapshot.settings;
    result = apply_settings_form(&form, &settings);
    if (result != ESP_OK) {
        return send_form_result(request, false, "400 Bad Request",
                                "Check latitude, longitude, radius, and options.");
    }
    result = s_save_settings(&settings, s_user_context);
    if (result != ESP_OK) {
        return send_form_result(request, false, "500 Internal Server Error",
                                "Settings were not saved.");
    }
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.settings = settings;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return send_form_result(request, true, "200 OK", "");
}

static esp_err_t reboot_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    if (s_reboot == NULL) {
        return send_form_result(request, false, "503 Service Unavailable",
                                "Restart is not available.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    settings_form_t form;
    if (result == ESP_OK) {
        result = decode_settings_form(body, received, &form);
    }
    memset(body, 0, sizeof(body));
    if (result != ESP_OK || !form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    result = s_reboot(s_user_context);
    if (result != ESP_OK) {
        return send_form_result(request, false, "409 Conflict",
                                "A restart is already scheduled.");
    }
    return send_form_result(request, true, "200 OK", "");
}

static esp_err_t aircraft_api_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot,
                                       "/api/v1/aircraft");
    }
    esp_err_t result = httpd_resp_set_type(request,
                                            "application/json; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        char head[320];
        char last_success[24];
        if (snapshot.aircraft.last_success_monotonic_ms > 0) {
            (void)snprintf(last_success, sizeof(last_success), "%.0f",
                           seconds_since_success(&snapshot));
        } else {
            memcpy(last_success, "null", sizeof("null"));
        }
        const int length = snprintf(
            head, sizeof(head),
            "{\"state\":\"%s\",\"error\":\"%s\",\"sequence\":%llu,"
            "\"reported\":%lu,\"accepted\":%lu,\"radius_nm\":%u,"
            "\"unit\":\"%s\",\"last_success_age_s\":%s,"
            "\"http_status\":%d,\"focus\":\"%s\",\"aircraft\":[",
            airtrack_feed_state_name(snapshot.aircraft.state),
            airtrack_feed_error_name(snapshot.aircraft.error),
            (unsigned long long)snapshot.aircraft.sequence,
            (unsigned long)snapshot.aircraft.aircraft_reported,
            (unsigned long)snapshot.aircraft.aircraft_accepted,
            (unsigned)snapshot.settings.radius_nm,
            distance_suffix(snapshot.settings.distance_unit),
            last_success, snapshot.aircraft.http_status,
            snapshot.settings.focus_flight);
        result = (length < 0 || (size_t)length >= sizeof(head))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, head, (size_t)length);
    }
    for (size_t index = 0U;
         result == ESP_OK && index < snapshot.aircraft.aircraft_count; ++index) {
        const airtrack_aircraft_t *aircraft = &snapshot.aircraft.aircraft[index];
        if (index > 0U) {
            result = send_html_chunk(request, ",");
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "{\"hex\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->hex);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "\",\"callsign\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->callsign);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "\",\"registration\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->registration);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "\",\"squawk\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->squawk);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "\",\"category\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->category);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "\",\"type\":\"");
        }
        if (result == ESP_OK) {
            result = send_json_escaped(request, aircraft->aircraft_type);
        }
        if (result == ESP_OK) {
            char route[80];
            float remaining_nm = -1.0f;
            const long eta_s = route_remaining(aircraft, &remaining_nm);
            char remaining[24] = "null";
            char eta[24] = "null";
            if (remaining_nm >= 0.0f) {
                (void)snprintf(remaining, sizeof(remaining), "%.1f", (double)remaining_nm);
            }
            if (eta_s >= 0) {
                (void)snprintf(eta, sizeof(eta), "%ld", eta_s);
            }
            const int length = snprintf(
                route, sizeof(route),
                "\",\"route_from\":\"%s\",\"route_to\":\"%s\","
                "\"remaining_nm\":%s,\"eta_s\":%s",
                aircraft->route_valid ? aircraft->route_from : "",
                aircraft->route_valid ? aircraft->route_to : "",
                remaining, eta);
            result = send_chunk_or_size(request, route, length, sizeof(route));
        }
        if (result == ESP_OK) {
            char numeric[448];
            const int length = snprintf(
                numeric, sizeof(numeric),
                ",\"emergency\":%s,"
                "\"ground\":%s,\"altitude_ft\":%ld,"
                "\"altitude_valid\":%s,\"distance_nm\":%.3f,"
                "\"bearing_deg\":%.1f,\"ground_speed_kt\":%.1f,"
                "\"speed_valid\":%s,\"track_deg\":%.1f,"
                "\"track_valid\":%s,\"vertical_rate_fpm\":%ld,"
                "\"vertical_rate_valid\":%s,\"seen_pos_s\":%.2f,"
                "\"age_s\":%.1f}",
                aircraft->emergency ? "true" : "false",
                aircraft->ground ? "true" : "false",
                (long)aircraft->altitude_ft,
                aircraft->altitude_valid ? "true" : "false",
                (double)aircraft->distance_nm, (double)aircraft->bearing_deg,
                (double)aircraft->ground_speed_kt,
                aircraft->ground_speed_valid ? "true" : "false",
                (double)aircraft->track_deg,
                aircraft->track_valid ? "true" : "false",
                (long)aircraft->vertical_rate_fpm,
                aircraft->vertical_rate_valid ? "true" : "false",
                (double)aircraft->seen_pos_s,
                current_position_age_s(&snapshot, aircraft));
            result = (length < 0 || (size_t)length >= sizeof(numeric))
                         ? ESP_ERR_INVALID_SIZE
                         : httpd_resp_send_chunk(request, numeric,
                                                 (size_t)length);
        }
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "]}");
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t logs_list_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/api/v1/logs");
    }
    storage_log_file_t files[STORAGE_LOG_MAX_LISTED];
    size_t count = 0U;
    uint64_t total = 0U;
    const esp_err_t listed = storage_logger_list(files, STORAGE_LOG_MAX_LISTED,
                                                 &count, &total);
    esp_err_t result = httpd_resp_set_type(request, "application/json; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        char head[96];
        const int length = snprintf(head, sizeof(head),
            "{\"mounted\":%s,\"total_bytes\":%llu,\"files\":[",
            listed != ESP_ERR_NOT_FOUND ? "true" : "false",
            (unsigned long long)total);
        result = send_chunk_or_size(request, head, length, sizeof(head));
    }
    for (size_t index = 0U; result == ESP_OK && listed == ESP_OK && index < count;
         ++index) {
        char item[80];
        const int length = snprintf(item, sizeof(item),
                                    "%s{\"name\":\"%s\",\"bytes\":%lu}",
                                    index > 0U ? "," : "", files[index].name,
                                    (unsigned long)files[index].bytes);
        result = send_chunk_or_size(request, item, length, sizeof(item));
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "]}");
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

#define LOG_READ_CHUNK_BYTES 2048U
#define LOG_TAIL_DEFAULT_BYTES (48U * 1024U)
#define LOG_TAIL_MAX_BYTES (256U * 1024U)

static esp_err_t log_file_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_plain_error(request, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    /* URI: /api/v1/logs/<name>[?tail=bytes][&download=1] */
    const char *name = request->uri + sizeof("/api/v1/logs/") - 1U;
    char clean[STORAGE_LOG_NAME_MAX_BYTES + 1U];
    size_t name_length = 0U;
    while (name[name_length] != '\0' && name[name_length] != '?' &&
           name_length < sizeof(clean) - 1U) {
        clean[name_length] = name[name_length];
        ++name_length;
    }
    clean[name_length] = '\0';
    if (!storage_logger_valid_name(clean)) {
        return send_plain_error(request, "404 Not Found", "No such log.");
    }
    size_t tail = LOG_TAIL_DEFAULT_BYTES;
    bool download = false;
    char query[64];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "tail", value, sizeof(value)) == ESP_OK) {
            unsigned long parsed = 0UL;
            if (parse_bounded_ulong(value, 1UL, LOG_TAIL_MAX_BYTES, &parsed)) {
                tail = (size_t)parsed;
            }
        }
        if (httpd_query_key_value(query, "download", value, sizeof(value)) == ESP_OK) {
            download = strcmp(value, "1") == 0;
            tail = LOG_TAIL_MAX_BYTES * 16U; /* whole file, bounded below */
        }
    }
    char *buffer = malloc(LOG_READ_CHUNK_BYTES);
    if (buffer == NULL) {
        return send_plain_error(request, "503 Service Unavailable", "Low memory.");
    }
    size_t read_bytes = 0U;
    size_t file_size = 0U;
    esp_err_t result = storage_logger_read(clean, 0U, buffer, 0U, &read_bytes,
                                           &file_size);
    if (result != ESP_OK) {
        free(buffer);
        return send_plain_error(request, result == ESP_ERR_TIMEOUT
                                             ? "503 Service Unavailable"
                                             : "404 Not Found",
                                result == ESP_ERR_TIMEOUT ? "SD card busy."
                                                          : "No such log.");
    }
    size_t offset = file_size > tail ? file_size - tail : 0U;
    result = httpd_resp_set_type(request, "application/x-ndjson; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK && download) {
        char disposition[80];
        (void)snprintf(disposition, sizeof(disposition),
                       "attachment; filename=\"airtrack-%s\"", clean);
        result = httpd_resp_set_hdr(request, "Content-Disposition", disposition);
    }
    if (result == ESP_OK) {
        char offset_text[16];
        (void)snprintf(offset_text, sizeof(offset_text), "%lu", (unsigned long)offset);
        result = httpd_resp_set_hdr(request, "X-Log-Offset", offset_text);
    }
    /* Skip a partial first line when tailing so every emitted line is whole. */
    bool skip_partial = offset > 0U;
    while (result == ESP_OK && offset < file_size) {
        result = storage_logger_read(clean, offset, buffer, LOG_READ_CHUNK_BYTES,
                                     &read_bytes, &file_size);
        if (result != ESP_OK || read_bytes == 0U) {
            break;
        }
        size_t start = 0U;
        if (skip_partial) {
            const char *newline = memchr(buffer, '\n', read_bytes);
            if (newline == NULL) {
                offset += read_bytes;
                continue;
            }
            start = (size_t)(newline - buffer) + 1U;
            skip_partial = false;
        }
        if (read_bytes > start) {
            result = httpd_resp_send_chunk(request, buffer + start, read_bytes - start);
        }
        offset += read_bytes;
    }
    free(buffer);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t logs_clear_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    settings_form_t form;
    if (result == ESP_OK) {
        result = decode_settings_form(body, received, &form);
    }
    memset(body, 0, sizeof(body));
    if (result != ESP_OK || !form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    uint32_t deleted = 0U;
    result = storage_logger_clear(&deleted);
    if (result == ESP_ERR_NOT_FOUND) {
        return send_form_result(request, false, "409 Conflict", "No SD card.");
    }
    if (result != ESP_OK) {
        return send_form_result(request, false, "500 Internal Server Error",
                                "Some log files could not be deleted.");
    }
    return send_form_result(request, true, "200 OK", "");
}

static esp_err_t factory_reset_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    if (s_factory_reset == NULL) {
        return send_form_result(request, false, "503 Service Unavailable",
                                "Factory reset is not available.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    settings_form_t form;
    if (result == ESP_OK) {
        result = decode_settings_form(body, received, &form);
    }
    memset(body, 0, sizeof(body));
    if (result != ESP_OK || !form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    if (!form.have_confirm || strcmp(form.confirm, "RESET") != 0) {
        return send_form_result(request, false, "400 Bad Request",
                                "Type RESET to confirm.");
    }
    result = s_factory_reset(s_user_context);
    if (result != ESP_OK) {
        return send_form_result(request, false, "500 Internal Server Error",
                                "Reset failed; nothing was changed.");
    }
    return send_form_result(request, true, "200 OK", "");
}

static const char *ota_state_name(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_CHECKING: return "checking";
    case OTA_STATE_UP_TO_DATE: return "up_to_date";
    case OTA_STATE_AVAILABLE: return "available";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFYING: return "verifying";
    case OTA_STATE_READY: return "ready";
    case OTA_STATE_FAILED: return "failed";
    default: return "idle";
    }
}

static esp_err_t send_ota_status_json(httpd_req_t *request)
{
    ota_status_t ota;
    (void)ota_get_status(&ota);
    esp_err_t result = httpd_resp_set_type(request, "application/json; charset=utf-8");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        char head[160];
        const int length = snprintf(head, sizeof(head),
            "{\"state\":\"%s\",\"busy\":%s,\"percent\":%u,\"downloaded\":%lu,"
            "\"size\":%lu,\"pending_verify\":%s,\"partition\":\"%s\",\"current\":\"",
            ota_state_name(ota.state), ota_busy() ? "true" : "false",
            (unsigned)ota.percent, (unsigned long)ota.downloaded,
            (unsigned long)ota.size, ota.pending_verify ? "true" : "false",
            ota.running_partition);
        result = send_chunk_or_size(request, head, length, sizeof(head));
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, ota.current_version);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\",\"available\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, ota.available_version);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\",\"notes\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, ota.notes);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\",\"error\":\"");
    }
    if (result == ESP_OK) {
        result = send_json_escaped(request, ota.error);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\"}");
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t ota_status_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/api/v1/ota/status");
    }
    return send_ota_status_json(request);
}

static esp_err_t ota_check_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    settings_form_t form;
    if (result == ESP_OK) {
        result = decode_settings_form(body, received, &form);
    }
    memset(body, 0, sizeof(body));
    if (result != ESP_OK || !form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    result = ota_check_async();
    if (result == ESP_ERR_INVALID_STATE) {
        return send_form_result(request, false, "409 Conflict",
                                "An update task is already running.");
    }
    if (result != ESP_OK) {
        return send_form_result(request, false, "503 Service Unavailable",
                                "Could not start the update check.");
    }
    return send_form_result(request, true, "200 OK", "");
}

static esp_err_t ota_start_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_form_result(request, false, "403 Forbidden",
                                "Use the address shown on the LCD.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    esp_err_t result = read_form_body(request, body, sizeof(body), &received);
    settings_form_t form;
    if (result == ESP_OK) {
        result = decode_settings_form(body, received, &form);
    }
    memset(body, 0, sizeof(body));
    if (result != ESP_OK || !form.csrf_ok) {
        return send_form_result(request, false, "403 Forbidden",
                                "Session token expired; reload the page.");
    }
    if (!form.have_version) {
        return send_form_result(request, false, "400 Bad Request",
                                "Missing version.");
    }
    result = ota_start(form.version);
    if (result == ESP_ERR_INVALID_STATE) {
        return send_form_result(request, false, "409 Conflict",
                                "Check for updates first; the offered version must match.");
    }
    if (result != ESP_OK) {
        return send_form_result(request, false, "503 Service Unavailable",
                                "Could not start the update.");
    }
    return send_form_result(request, true, "200 OK", "");
}

static esp_err_t favicon_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/favicon.ico");
    }

    esp_err_t result = httpd_resp_set_status(request, "204 No Content");
    if (result == ESP_OK) {
        result = set_security_headers(request);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send(request, NULL, 0U);
    }
    return result;
}

static const httpd_uri_t URI_ROOT = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = status_page_handler,
};

static const httpd_uri_t URI_STATUS = {
    .uri = "/api/v1/status",
    .method = HTTP_GET,
    .handler = status_api_handler,
};

static const httpd_uri_t URI_CONFIG = {
    .uri = "/api/v1/config",
    .method = HTTP_GET,
    .handler = config_api_handler,
};

static const httpd_uri_t URI_AIRCRAFT = {
    .uri = "/api/v1/aircraft",
    .method = HTTP_GET,
    .handler = aircraft_api_handler,
};

static const httpd_uri_t URI_SETTINGS = {
    .uri = "/api/v1/config",
    .method = HTTP_POST,
    .handler = settings_form_handler,
};

static const httpd_uri_t URI_REBOOT = {
    .uri = "/api/v1/reboot",
    .method = HTTP_POST,
    .handler = reboot_handler,
};

static const httpd_uri_t URI_FACTORY_RESET = {
    .uri = "/api/v1/factory-reset",
    .method = HTTP_POST,
    .handler = factory_reset_handler,
};

static const httpd_uri_t URI_OTA_STATUS = {
    .uri = "/api/v1/ota/status",
    .method = HTTP_GET,
    .handler = ota_status_handler,
};

static const httpd_uri_t URI_OTA_CHECK = {
    .uri = "/api/v1/ota/check",
    .method = HTTP_POST,
    .handler = ota_check_handler,
};

static const httpd_uri_t URI_OTA_START = {
    .uri = "/api/v1/ota/start",
    .method = HTTP_POST,
    .handler = ota_start_handler,
};

static const httpd_uri_t URI_LOGS_CLEAR = {
    .uri = "/api/v1/logs/clear",
    .method = HTTP_POST,
    .handler = logs_clear_handler,
};

static const httpd_uri_t URI_LOGS = {
    .uri = "/api/v1/logs",
    .method = HTTP_GET,
    .handler = logs_list_handler,
};

static const httpd_uri_t URI_LOG_FILE = {
    .uri = "/api/v1/logs/*",
    .method = HTTP_GET,
    .handler = log_file_handler,
};

static const httpd_uri_t URI_FAVICON = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = favicon_handler,
};

static const httpd_uri_t URI_APP_JS = {
    .uri = "/app.js",
    .method = HTTP_GET,
    .handler = app_js_handler,
};

static const httpd_uri_t URI_APP_CSS = {
    .uri = "/app.css",
    .method = HTTP_GET,
    .handler = app_css_handler,
};

esp_err_t status_web_start(const status_web_snapshot_t *snapshot,
                           status_web_save_settings_cb_t save_settings,
                           status_web_reboot_cb_t reboot,
                           status_web_factory_reset_cb_t factory_reset,
                           void *user_context)
{
    status_web_snapshot_storage_t normalized;
    esp_err_t result = normalize_snapshot(snapshot, &normalized);
    if (result != ESP_OK || save_settings == NULL) {
        if (result == ESP_OK) {
            result = ESP_ERR_INVALID_ARG;
        }
        return result;
    }

    SemaphoreHandle_t lock = lifecycle_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_server != NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    replace_snapshot(&normalized);
    s_save_settings = save_settings;
    s_reboot = reboot;
    s_factory_reset = factory_reset;
    s_user_context = user_context;
    make_csrf_token();

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 10240U;
    server_config.max_open_sockets = 3U;
    server_config.max_uri_handlers = 24U;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 5U;
    server_config.send_wait_timeout = 5U;

    httpd_handle_t server = NULL;
    result = httpd_start(&server, &server_config);
    if (result != ESP_OK) {
        clear_snapshot();
        s_save_settings = NULL;
        s_reboot = NULL;
        s_factory_reset = NULL;
        s_user_context = NULL;
        memset(s_csrf_token, 0, sizeof(s_csrf_token));
        xSemaphoreGive(lock);
        return result;
    }

    const httpd_uri_t *handlers[] = {
        &URI_ROOT,
        &URI_STATUS,
        &URI_CONFIG,
        &URI_AIRCRAFT,
        &URI_SETTINGS,
        &URI_REBOOT,
        &URI_LOGS,
        &URI_LOGS_CLEAR,
        &URI_OTA_STATUS,
        &URI_OTA_CHECK,
        &URI_OTA_START,
        &URI_FACTORY_RESET,
        &URI_LOG_FILE,
        &URI_FAVICON,
        &URI_APP_JS,
        &URI_APP_CSS,
    };
    for (size_t index = 0U; index < sizeof(handlers) / sizeof(handlers[0]);
         ++index) {
        result = httpd_register_uri_handler(server, handlers[index]);
        if (result != ESP_OK) {
            const esp_err_t stop_result = httpd_stop(server);
            if (stop_result == ESP_OK) {
                clear_snapshot();
                s_save_settings = NULL;
        s_reboot = NULL;
        s_factory_reset = NULL;
                s_user_context = NULL;
                memset(s_csrf_token, 0, sizeof(s_csrf_token));
            } else {
                s_server = server;
                ESP_LOGE(TAG, "Could not stop partially configured server: %s",
                         esp_err_to_name(stop_result));
            }
            xSemaphoreGive(lock);
            return result;
        }
    }

    s_server = server;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "LAN status server started");
    return ESP_OK;
}

esp_err_t status_web_update(const status_web_snapshot_t *snapshot)
{
    status_web_snapshot_storage_t normalized;
    esp_err_t result = normalize_snapshot(snapshot, &normalized);
    if (result != ESP_OK) {
        return result;
    }

    SemaphoreHandle_t lock = lifecycle_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_server == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    replace_snapshot(&normalized);
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t status_web_stop(void)
{
    SemaphoreHandle_t lock = lifecycle_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_server == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = httpd_stop(s_server);
    if (result == ESP_OK) {
        s_server = NULL;
        clear_snapshot();
        s_save_settings = NULL;
        s_reboot = NULL;
        s_factory_reset = NULL;
        s_user_context = NULL;
        memset(s_csrf_token, 0, sizeof(s_csrf_token));
        ESP_LOGI(TAG, "LAN status server stopped");
    }
    xSemaphoreGive(lock);
    return result;
}

bool status_web_is_running(void)
{
    SemaphoreHandle_t lock = lifecycle_lock();
    if (lock == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool running = s_server != NULL;
    xSemaphoreGive(lock);
    return running;
}
