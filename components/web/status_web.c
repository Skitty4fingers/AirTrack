#include "status_web.h"

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
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
     sizeof("/api/v1/config/location"))
#define STATUS_WEB_CSRF_BYTES 32U
#define STATUS_WEB_FORM_MAX_BYTES 256U

static const char *TAG = "status_web";

typedef struct {
    char ssid[STATUS_WEB_SSID_MAX_BYTES + 1U];
    char ip_address[STATUS_WEB_IPV4_TEXT_MAX_BYTES + 1U];
    bool rssi_available;
    int8_t rssi_dbm;
    bool sd_mounted;
    uint32_t flash_bytes;
    uint32_t uptime_s;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    bool time_synchronized;
    airtrack_settings_t settings;
    airtrack_snapshot_t aircraft;
} status_web_snapshot_storage_t;

static httpd_handle_t s_server;
static status_web_snapshot_storage_t s_snapshot;
static status_web_save_location_cb_t s_save_location;
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

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AirTrack</title><style>"
    ":root{color-scheme:dark;font-family:system-ui,-apple-system,sans-serif;"
    "background:#071017;color:#eef8fb}*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px;"
    "background:radial-gradient(circle at top,#173544,#071017 58%)}"
    "main{width:min(100%,440px)}h1{font-size:2rem;letter-spacing:.04em;"
    "margin:.15em 0}.eyebrow{color:#63e6be;font-weight:750;letter-spacing:.18em;"
    "font-size:.74rem}.lead{color:#b9cbd2;line-height:1.5;margin:.4rem 0 1.2rem}"
    ".card{background:#0e2029;border:1px solid #28414c;border-radius:18px;"
    "padding:18px;box-shadow:0 20px 60px #0008;margin:12px 0}"
    ".grid{display:grid;grid-template-columns:auto minmax(0,1fr);gap:11px 16px;"
    "align-items:baseline}.grid span{color:#8faab5}.grid strong,.grid code{"
    "overflow-wrap:anywhere;color:#fff}.ok{color:#63e6be!important}"
    "label{display:block;color:#b9cbd2;font-size:.84rem;margin:12px 0 5px}"
    "input{width:100%;padding:11px;border:1px solid #38535f;border-radius:10px;"
    "font:inherit;color:#fff;background:#07141a}button{width:100%;margin-top:16px;"
    "padding:12px;border:0;border-radius:10px;font:inherit;font-weight:800;"
    "color:#05241c;background:#63e6be}"
    ".hint,footer{color:#8faab5;font-size:.8rem;line-height:1.5}"
    "footer{text-align:center;margin-top:15px}footer strong{color:#b9cbd2}"
    "</style></head><body><main><div class=eyebrow>LOCAL STATUS</div>"
    "<h1>AirTrack is online</h1><p class=lead>This display is connected to your "
    "Wi-Fi network and ready for the aircraft-tracking service.</p>"
    "<section class=card><div class=eyebrow>NETWORK</div><div class=grid>"
    "<span>Status</span><strong class=ok>Connected</strong>"
    "<span>SSID</span><strong>";

static const char PAGE_AFTER_SSID[] =
    "</strong><span>Address</span><code>";

static const char PAGE_AFTER_IP[] =
    "</code><span>Signal</span><strong>";

static const char PAGE_SYSTEM[] =
    "</strong></div></section><section class=card><div class=eyebrow>SYSTEM"
    "</div><div class=grid><span>Firmware</span><strong>";

static const char PAGE_AFTER_VERSION[] =
    "</strong><span>SD card</span><strong>";

static const char PAGE_AFTER_SD[] =
    "</strong><span>Flash</span><strong>";

static const char PAGE_TAIL[] =
    "</strong></div></section>";

static const char PAGE_FOOTER[] =
    "<footer><strong>Data: <a href=https://adsb.fi>adsb.fi</a></strong><br>"
    "Personal, non-commercial use. Not for navigation or collision "
    "avoidance.</footer></main></body></html>";

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
    destination->flash_bytes = source->flash_bytes;
    destination->uptime_s = source->uptime_s;
    destination->free_heap_bytes = source->free_heap_bytes;
    destination->minimum_free_heap_bytes = source->minimum_free_heap_bytes;
    destination->time_synchronized = source->time_synchronized;
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
            "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
            "base-uri 'none'; frame-ancestors 'none'");
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

static double current_position_age_s(
    const status_web_snapshot_storage_t *snapshot,
    const airtrack_aircraft_t *aircraft)
{
    double age = aircraft->seen_pos_s;
    const int64_t current_ms = (int64_t)snapshot->uptime_s * 1000LL;
    if (aircraft != NULL && snapshot->aircraft.updated_monotonic_ms > 0 &&
        current_ms > snapshot->aircraft.updated_monotonic_ms) {
        age += (double)(current_ms - snapshot->aircraft.updated_monotonic_ms) /
               1000.0;
    }
    return age;
}

static esp_err_t send_tracking_cards(
    httpd_req_t *request, const status_web_snapshot_storage_t *snapshot)
{
    esp_err_t result = send_html_chunk(
        request, "<section class=card><div class=eyebrow>NEAREST AIRCRAFT</div>");
    if (result != ESP_OK) {
        return result;
    }
    if (!snapshot->settings.location_configured) {
        result = send_html_chunk(request,
            "<h2>Set tracking location</h2><p class=hint>Enter this display's "
            "fixed decimal-degree coordinates. Wi-Fi credentials cannot be "
            "viewed or changed here.</p><form method=post "
            "action=/api/v1/config/location autocomplete=off>"
            "<input type=hidden name=csrf_token value=\"");
        if (result == ESP_OK) {
            result = send_html_escaped(request, s_csrf_token);
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request,
                "\"><label for=latitude>Latitude</label>"
                "<input id=latitude name=latitude type=number step=any min=-90 "
                "max=90 placeholder=37.6213 required>"
                "<label for=longitude>Longitude</label>"
                "<input id=longitude name=longitude type=number step=any "
                "min=-180 max=180 placeholder=-122.3790 required>"
                "<label for=radius>Radius (nautical miles)</label>"
                "<input id=radius name=radius type=number min=1 max=250 "
                "value=25 required><button type=submit>Start tracking</button>"
                "</form>");
        }
    } else if (snapshot->aircraft.aircraft_count == 0U) {
        char message[160];
        const int length = snprintf(
            message, sizeof(message), "<h2>%s</h2><p class=hint>Feed: %s",
            snapshot->aircraft.state == AIRTRACK_FEED_EMPTY
                ? "No recent aircraft" : "Waiting for aircraft data",
            airtrack_feed_state_name(snapshot->aircraft.state));
        result = (length < 0 || (size_t)length >= sizeof(message))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, message, (size_t)length);
        if (result == ESP_OK && snapshot->aircraft.error != AIRTRACK_ERROR_NONE) {
            result = send_html_chunk(request, " &middot; ");
        }
        if (result == ESP_OK && snapshot->aircraft.error != AIRTRACK_ERROR_NONE) {
            result = send_html_escaped(
                request, airtrack_feed_error_name(snapshot->aircraft.error));
        }
        if (result == ESP_OK) {
            result = send_html_chunk(request, "</p>");
        }
    } else {
        const airtrack_aircraft_t *aircraft = &snapshot->aircraft.aircraft[0];
        result = send_html_chunk(request, "<h2>");
        if (result == ESP_OK) {
            result = send_html_escaped(request, aircraft_identity(aircraft));
        }
        char altitude[32];
        char speed[32];
        if (aircraft->ground) {
            memcpy(altitude, "GROUND", sizeof("GROUND"));
        } else if (aircraft->altitude_valid) {
            (void)snprintf(altitude, sizeof(altitude), "%ld ft",
                           (long)aircraft->altitude_ft);
        } else {
            memcpy(altitude, "--", sizeof("--"));
        }
        if (aircraft->ground_speed_valid) {
            (void)snprintf(speed, sizeof(speed), "%.0f kt",
                           (double)aircraft->ground_speed_kt);
        } else {
            memcpy(speed, "--", sizeof("--"));
        }
        char details[384];
        const int length = snprintf(
            details, sizeof(details),
            "</h2><div class=grid><span>Distance</span><strong>%.1f %s</strong>"
            "<span>Bearing</span><strong>%03.0f&deg; true</strong>"
            "<span>Altitude</span><strong>%s</strong>"
            "<span>Speed</span><strong>%s</strong>"
            "<span>Position age</span><strong>%.1f s</strong>"
            "<span>Feed</span><strong>%s</strong></div>",
            (double)display_distance(aircraft->distance_nm,
                                     snapshot->settings.distance_unit),
            distance_suffix(snapshot->settings.distance_unit),
            (double)aircraft->bearing_deg,
            altitude, speed,
            current_position_age_s(snapshot, aircraft),
            airtrack_feed_state_name(snapshot->aircraft.state));
        if (length < 0 || (size_t)length >= sizeof(details)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request, details, (size_t)length);
        }
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</section>");
    }
    if (result == ESP_OK && snapshot->settings.location_configured) {
        result = send_html_chunk(request, "<section class=card>"
            "<div class=eyebrow>TRACKING SETTINGS</div><div class=grid>");
    }
    if (result == ESP_OK && snapshot->settings.location_configured) {
        char settings[1024];
        const int length = snprintf(
            settings, sizeof(settings),
            "<span>Latitude</span><strong>%.6f</strong>"
            "<span>Longitude</span><strong>%.6f</strong>"
            "<span>Radius</span><strong>%u NM</strong>"
            "<span>Poll interval</span><strong>%u s</strong>"
            "<span>Ground aircraft</span><strong>%s</strong>"
            "</div><p class=hint>Hold BOOT for five seconds to reopen secure "
            "setup and change Wi-Fi or tracking settings.</p></section>",
            (double)snapshot->settings.latitude_e7 / 10000000.0,
            (double)snapshot->settings.longitude_e7 / 10000000.0,
            (unsigned)snapshot->settings.radius_nm,
            (unsigned)snapshot->settings.poll_interval_s,
            snapshot->settings.include_ground ? "Included" : "Excluded");
        result = (length < 0 || (size_t)length >= sizeof(settings))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, settings, (size_t)length);
    }
    return result;
}

static esp_err_t status_page_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_canonical_redirect(request, &snapshot, "/");
    }

    char version[STATUS_WEB_VERSION_MAX_BYTES + 1U];
    copy_firmware_version(version);

    char signal[24];
    if (snapshot.rssi_available) {
        const int length = snprintf(signal, sizeof(signal), "%d dBm",
                                    (int)snapshot.rssi_dbm);
        if (length < 0 || (size_t)length >= sizeof(signal)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        memcpy(signal, "Unavailable", sizeof("Unavailable"));
    }

    char flash[48];
    const uint32_t mib = snapshot.flash_bytes / (1024U * 1024U);
    int flash_length;
    if (snapshot.flash_bytes > 0U &&
        snapshot.flash_bytes % (1024U * 1024U) == 0U) {
        flash_length = snprintf(flash, sizeof(flash), "%lu MiB (%lu bytes)",
                                (unsigned long)mib,
                                (unsigned long)snapshot.flash_bytes);
    } else {
        flash_length = snprintf(flash, sizeof(flash), "%lu bytes",
                                (unsigned long)snapshot.flash_bytes);
    }
    if (flash_length < 0 || (size_t)flash_length >= sizeof(flash)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result =
        httpd_resp_set_type(request, "text/html; charset=utf-8");
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
        result = send_html_escaped(request, snapshot.ip_address);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_IP);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, signal);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_SYSTEM);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, version);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_VERSION);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request,
                                 snapshot.sd_mounted ? "Mounted" : "Unavailable");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_SD);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, flash);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_TAIL);
    }
    if (result == ESP_OK) {
        result = send_tracking_cards(request, &snapshot);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FOOTER);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
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

        char tail[320];
        const int length = snprintf(
            tail, sizeof(tail),
            "\",\"rssi_dbm\":%s,\"sd_mounted\":%s,\"flash_bytes\":%lu,"
            "\"uptime_s\":%lu,\"free_heap_bytes\":%lu,"
            "\"minimum_free_heap_bytes\":%lu,\"time_synchronized\":%s,"
            "\"feed_state\":\"%s\",\"feed_error\":\"%s\","
            "\"aircraft_count\":%u,\"http_status\":%d}",
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
            snapshot.aircraft.http_status);
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
    char body[512];
    const int length = snprintf(
        body, sizeof(body),
        "{\"generation\":%llu,\"location_configured\":%s,"
        "\"latitude\":%.7f,\"longitude\":%.7f,\"radius_nm\":%u,"
        "\"poll_interval_s\":%u,\"max_position_age_s\":%u,"
        "\"include_ground\":%s,\"distance_unit\":\"%s\","
        "\"brightness_percent\":%u,\"logging_mode\":%u}",
        (unsigned long long)snapshot.settings.generation,
        snapshot.settings.location_configured ? "true" : "false",
        (double)snapshot.settings.latitude_e7 / 10000000.0,
        (double)snapshot.settings.longitude_e7 / 10000000.0,
        (unsigned)snapshot.settings.radius_nm,
        (unsigned)snapshot.settings.poll_interval_s,
        (unsigned)snapshot.settings.max_position_age_s,
        snapshot.settings.include_ground ? "true" : "false",
        distance_suffix(snapshot.settings.distance_unit),
        (unsigned)snapshot.settings.brightness_percent,
        (unsigned)snapshot.settings.logging_mode);
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

static esp_err_t parse_location_form(const char *body, size_t length,
                                     int32_t *latitude_e7,
                                     int32_t *longitude_e7,
                                     uint16_t *radius_nm)
{
    char latitude[24] = {0};
    char longitude[24] = {0};
    char radius[8] = {0};
    char csrf[STATUS_WEB_CSRF_BYTES + 1U] = {0};
    bool have_latitude = false;
    bool have_longitude = false;
    bool have_radius = false;
    bool have_csrf = false;
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
        const char *encoded = equals + 1;
        const size_t encoded_length = pair_length - (size_t)(encoded - pair);
        if (result == ESP_OK && strcmp(key, "latitude") == 0 && !have_latitude) {
            result = decode_form(encoded, encoded_length, latitude,
                                 sizeof(latitude));
            have_latitude = result == ESP_OK;
        } else if (result == ESP_OK && strcmp(key, "longitude") == 0 &&
                   !have_longitude) {
            result = decode_form(encoded, encoded_length, longitude,
                                 sizeof(longitude));
            have_longitude = result == ESP_OK;
        } else if (result == ESP_OK && strcmp(key, "radius") == 0 &&
                   !have_radius) {
            result = decode_form(encoded, encoded_length, radius,
                                 sizeof(radius));
            have_radius = result == ESP_OK;
        } else if (result == ESP_OK && strcmp(key, "csrf_token") == 0 &&
                   !have_csrf) {
            result = decode_form(encoded, encoded_length, csrf, sizeof(csrf));
            have_csrf = result == ESP_OK;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        if (result != ESP_OK) {
            return result;
        }
        offset += pair_length + (separator != NULL ? 1U : 0U);
    }
    uint8_t csrf_difference = 0U;
    if (!have_latitude || !have_longitude || !have_radius || !have_csrf ||
        strlen(csrf) != STATUS_WEB_CSRF_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < STATUS_WEB_CSRF_BYTES; ++index) {
        csrf_difference |= (uint8_t)csrf[index] ^ (uint8_t)s_csrf_token[index];
    }
    if (csrf_difference != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    errno = 0;
    char *latitude_end = NULL;
    char *longitude_end = NULL;
    char *radius_end = NULL;
    const double latitude_value = strtod(latitude, &latitude_end);
    const double longitude_value = strtod(longitude, &longitude_end);
    const unsigned long radius_value = strtoul(radius, &radius_end, 10);
    if (errno != 0 || latitude_end == latitude || *latitude_end != '\0' ||
        longitude_end == longitude || *longitude_end != '\0' ||
        radius_end == radius || *radius_end != '\0' ||
        !isfinite(latitude_value) || !isfinite(longitude_value) ||
        latitude_value < -90.0 || latitude_value > 90.0 ||
        longitude_value < -180.0 || longitude_value > 180.0 ||
        radius_value < 1UL || radius_value > 250UL) {
        return ESP_ERR_INVALID_ARG;
    }
    *latitude_e7 = (int32_t)llround(latitude_value * 10000000.0);
    *longitude_e7 = (int32_t)llround(longitude_value * 10000000.0);
    *radius_nm = (uint16_t)radius_value;
    return ESP_OK;
}

static esp_err_t location_form_handler(httpd_req_t *request)
{
    const status_web_snapshot_storage_t snapshot = copy_snapshot();
    if (!request_has_canonical_host(request, &snapshot)) {
        return send_plain_error(
            request, "403 Forbidden",
            "Use the AirTrack address shown on the LCD.");
    }
    if (snapshot.settings.location_configured || s_save_location == NULL) {
        return send_plain_error(request, "409 Conflict",
            "Location is already configured. Hold BOOT for five seconds to change it.");
    }
    if (!form_content_type(request) || request->content_len <= 0 ||
        (size_t)request->content_len > STATUS_WEB_FORM_MAX_BYTES) {
        return send_plain_error(request, "400 Bad Request",
                                "Invalid location form.");
    }
    char body[STATUS_WEB_FORM_MAX_BYTES + 1U];
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + received,
            (size_t)request->content_len - received);
        if (count <= 0) {
            memset(body, 0, sizeof(body));
            return send_plain_error(request, "408 Request Timeout",
                                    "Incomplete location form.");
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint16_t radius_nm;
    const esp_err_t parsed = parse_location_form(
        body, received, &latitude_e7, &longitude_e7, &radius_nm);
    memset(body, 0, sizeof(body));
    if (parsed != ESP_OK) {
        return send_plain_error(request, "400 Bad Request",
            "Check latitude, longitude, radius, and reload the form.");
    }
    const esp_err_t saved = s_save_location(
        latitude_e7, longitude_e7, radius_nm, s_user_context);
    if (saved != ESP_OK) {
        return send_plain_error(request, "500 Internal Server Error",
                                "Settings were not saved.");
    }
    (void)set_security_headers(request);
    (void)httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request,
        "<!doctype html><meta name=viewport content=\"width=device-width\">"
        "<title>AirTrack configured</title><body style=\"font-family:system-ui;"
        "background:#071017;color:#eef8fb;padding:2rem\"><h1>Location saved</h1>"
        "<p>AirTrack is restarting and will begin tracking nearby aircraft.</p></body>");
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
        char head[192];
        const int length = snprintf(
            head, sizeof(head),
            "{\"state\":\"%s\",\"error\":\"%s\",\"sequence\":%llu,"
            "\"reported\":%lu,\"accepted\":%lu,\"aircraft\":[",
            airtrack_feed_state_name(snapshot.aircraft.state),
            airtrack_feed_error_name(snapshot.aircraft.error),
            (unsigned long long)snapshot.aircraft.sequence,
            (unsigned long)snapshot.aircraft.aircraft_reported,
            (unsigned long)snapshot.aircraft.aircraft_accepted);
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
            char numeric[384];
            const int length = snprintf(
                numeric, sizeof(numeric),
                "\",\"type\":\"%s\",\"ground\":%s,\"altitude_ft\":%ld,"
                "\"altitude_valid\":%s,\"distance_nm\":%.3f,"
                "\"bearing_deg\":%.1f,\"ground_speed_kt\":%.1f,"
                "\"speed_valid\":%s,\"track_deg\":%.1f,"
                "\"track_valid\":%s,\"vertical_rate_fpm\":%ld,"
                "\"vertical_rate_valid\":%s,\"seen_pos_s\":%.2f}",
                aircraft->aircraft_type, aircraft->ground ? "true" : "false",
                (long)aircraft->altitude_ft,
                aircraft->altitude_valid ? "true" : "false",
                (double)aircraft->distance_nm, (double)aircraft->bearing_deg,
                (double)aircraft->ground_speed_kt,
                aircraft->ground_speed_valid ? "true" : "false",
                (double)aircraft->track_deg,
                aircraft->track_valid ? "true" : "false",
                (long)aircraft->vertical_rate_fpm,
                aircraft->vertical_rate_valid ? "true" : "false",
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

static const httpd_uri_t URI_LOCATION = {
    .uri = "/api/v1/config/location",
    .method = HTTP_POST,
    .handler = location_form_handler,
};

static const httpd_uri_t URI_FAVICON = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = favicon_handler,
};

esp_err_t status_web_start(const status_web_snapshot_t *snapshot,
                           status_web_save_location_cb_t save_location,
                           void *user_context)
{
    status_web_snapshot_storage_t normalized;
    esp_err_t result = normalize_snapshot(snapshot, &normalized);
    if (result != ESP_OK || save_location == NULL) {
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
    s_save_location = save_location;
    s_user_context = user_context;
    make_csrf_token();

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 5120U;
    server_config.max_open_sockets = 3U;
    server_config.max_uri_handlers = 6U;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 5U;
    server_config.send_wait_timeout = 5U;

    httpd_handle_t server = NULL;
    result = httpd_start(&server, &server_config);
    if (result != ESP_OK) {
        clear_snapshot();
        s_save_location = NULL;
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
        &URI_LOCATION,
        &URI_FAVICON,
    };
    for (size_t index = 0U; index < sizeof(handlers) / sizeof(handlers[0]);
         ++index) {
        result = httpd_register_uri_handler(server, handlers[index]);
        if (result != ESP_OK) {
            const esp_err_t stop_result = httpd_stop(server);
            if (stop_result == ESP_OK) {
                clear_snapshot();
                s_save_location = NULL;
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
        s_save_location = NULL;
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
