#include "setup_web.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"

#define SETUP_WEB_MAX_FORM_BYTES 768U
#define SETUP_WEB_CONTENT_TYPE_MAX_BYTES 63U
#define SETUP_WEB_CSRF_RANDOM_BYTES 16U
#define SETUP_WEB_CSRF_TOKEN_BYTES (SETUP_WEB_CSRF_RANDOM_BYTES * 2U)
#define SETUP_WEB_CANONICAL_URL_MAX_BYTES \
    (sizeof("http://") + SETUP_WEB_IPV4_TEXT_MAX_BYTES + 1U)
#define SETUP_WEB_CANONICAL_ACTION_MAX_BYTES                                \
    (sizeof("http://") - 1U + SETUP_WEB_IPV4_TEXT_MAX_BYTES +              \
     sizeof("/api/v1/config/wifi"))
#define SETUP_WEB_CANONICAL_HOST_MAX_BYTES \
    (SETUP_WEB_IPV4_TEXT_MAX_BYTES + sizeof(":80"))

static const char *TAG = "setup_web";

typedef struct {
    httpd_handle_t server;
    char ap_ssid[SETUP_WEB_SSID_MAX_BYTES + 1U];
    char ap_password[SETUP_WEB_PASSWORD_MAX_BYTES + 1U];
    char ap_ip_address[SETUP_WEB_IPV4_TEXT_MAX_BYTES + 1U];
    char canonical_url[SETUP_WEB_CANONICAL_URL_MAX_BYTES];
    char canonical_action[SETUP_WEB_CANONICAL_ACTION_MAX_BYTES];
    char csrf_token[SETUP_WEB_CSRF_TOKEN_BYTES + 1U];
    setup_web_network_t nearby_networks[SETUP_WEB_MAX_NETWORKS];
    size_t nearby_network_count;
    char current_ssid[SETUP_WEB_SSID_MAX_BYTES + 1U];
    airtrack_settings_t current_settings;
    setup_web_save_config_cb_t save_config;
    void *user_context;
} setup_web_context_t;

static setup_web_context_t s_web;

static void clear_sensitive_buffer(void *buffer, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    while (length-- > 0U) {
        *bytes++ = 0U;
    }
}

static void make_csrf_token(char token[SETUP_WEB_CSRF_TOKEN_BYTES + 1U])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[SETUP_WEB_CSRF_RANDOM_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t index = 0U; index < sizeof(random_bytes); ++index) {
        token[index * 2U] = hex[random_bytes[index] >> 4U];
        token[(index * 2U) + 1U] = hex[random_bytes[index] & 0x0fU];
    }
    token[SETUP_WEB_CSRF_TOKEN_BYTES] = '\0';
    clear_sensitive_buffer(random_bytes, sizeof(random_bytes));
}

static bool valid_csrf_token_text(const char *token)
{
    if (strnlen(token, SETUP_WEB_CSRF_TOKEN_BYTES + 1U) !=
        SETUP_WEB_CSRF_TOKEN_BYTES) {
        return false;
    }
    for (size_t index = 0U; index < SETUP_WEB_CSRF_TOKEN_BYTES; ++index) {
        if (!((token[index] >= '0' && token[index] <= '9') ||
              (token[index] >= 'a' && token[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool csrf_token_matches(const char *candidate)
{
    volatile uint8_t difference = 0U;
    for (size_t index = 0U; index < SETUP_WEB_CSRF_TOKEN_BYTES; ++index) {
        difference |= (uint8_t)candidate[index] ^
                      (uint8_t)s_web.csrf_token[index];
    }
    return difference == 0U;
}

#define SETUP_ICON_PLANE "<svg viewBox=\"0 0 24 24\"><path d=\"M21 16v-2l-8-5V3.5a1.5 1.5 0 0 0-3 0V9l-8 5v2l8-2.5V19l-2 1.5V22l3.5-1 3.5 1v-1.5L13 19v-5.5z\"/></svg>"
#define SETUP_ICON_WIFI "<svg viewBox=\"0 0 24 24\"><path d=\"M12 21l3.5-4.7a5.9 5.9 0 0 0-7 0zm0-9a10 10 0 0 0-6.4 2.3l1.8 2.4a7 7 0 0 1 9.2 0l1.8-2.4A10 10 0 0 0 12 12zm0-6A16 16 0 0 0 1.5 9.9l1.8 2.4a13 13 0 0 1 17.4 0l1.8-2.4A16 16 0 0 0 12 6z\"/></svg>"
#define SETUP_ICON_SAVE "<svg viewBox=\"0 0 24 24\"><path d=\"M17 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V7zm-5 16a3 3 0 1 1 0-6 3 3 0 0 1 0 6zm3-10H5V5h10z\"/></svg>"

/* Same visual language as the LAN dashboard (components/web/assets/app.css),
 * inlined because the captive portal must be a single self-contained page. */
static const char SETUP_CSS[] =
    ":root{color-scheme:light;font-family:system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;"
    "--navy:#0d2a4d;--blue:#1e88e5;--blue2:#1976d2;--ink:#12213a;--muted:#5d6b80;"
    "--line:#e3e8ef;--bg:#f3f5f8}*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--ink);min-height:100vh}"
    ".top{background:var(--navy);color:#fff;padding:16px 20px;display:flex;align-items:center;gap:10px;"
    "font-size:1.35rem;font-weight:800}.top svg{width:26px;height:26px;fill:#fff}"
    ".top small{margin-left:auto;font-size:.8rem;font-weight:600;color:#c9d6e8;letter-spacing:.06em}"
    "main{width:min(100%,520px);margin:0 auto;padding:18px 16px 24px}"
    "h1{font-size:1.5rem;margin:6px 0 4px}.lead{color:var(--muted);line-height:1.5;margin:0 0 16px}"
    ".card{background:#fff;border:1px solid var(--line);border-radius:12px;padding:20px 22px;"
    "box-shadow:0 1px 2px #0d2a4d0f;margin:14px 0}"
    "h2{margin:0 0 12px;font-size:1.12rem;font-weight:700;display:flex;align-items:center;gap:8px}"
    "h2 svg{width:20px;height:20px;fill:var(--blue)}"
    "h3{margin:18px 0 8px;font-size:.98rem;font-weight:600}"
    ".join{display:grid;grid-template-columns:auto 1fr;gap:9px 16px;font-size:.98rem}"
    ".join span{color:var(--muted)}.join code{overflow-wrap:anywhere;font-size:1.05rem;font-weight:700}"
    ".join code.key{color:var(--blue)}"
    "label{display:block;font-size:.92rem;color:var(--muted);margin:14px 0 6px}"
    "input,select{width:100%;padding:11px 12px;border:1px solid #c9d3e0;border-radius:8px;"
    "font:inherit;color:var(--ink);background:#fff}"
    "input:focus,select:focus{outline:2px solid #1e88e555;border-color:var(--blue)}"
    "select{white-space:nowrap;text-overflow:ellipsis}.manual{color:var(--blue);font-weight:600}"
    "button{width:100%;margin-top:18px;border:1px solid var(--blue);border-radius:8px;padding:13px;"
    "font:inherit;font-weight:700;color:#fff;background:var(--blue);cursor:pointer;"
    "display:inline-flex;align-items:center;justify-content:center;gap:8px}"
    "button svg{width:18px;height:18px;fill:currentColor}button:hover{background:var(--blue2)}"
    ".hint,footer{color:var(--muted);font-size:.86rem;line-height:1.5}.hint{margin:8px 0 0}"
    "footer{text-align:center;margin-top:16px}"
    "details{margin-top:18px;border-top:1px solid var(--line);padding-top:12px}"
    "summary{cursor:pointer;color:var(--blue);font-weight:700;font-size:.95rem}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}"
    ".check{display:flex;align-items:center;gap:10px;margin:14px 0 6px;color:var(--ink);"
    "font-size:.95rem}.check input{width:auto;margin:0;accent-color:var(--blue)}"
    ".two{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}"
    "a{color:var(--blue)}";

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AirTrack setup</title><style>";

static const char PAGE_AFTER_CSS[] =
    "</style></head><body><div class=top>" SETUP_ICON_PLANE "AirTrack"
    "<small>LOCAL DEVICE SETUP</small></div><main>"
    "<h1>Connect this display</h1><p class=lead>Choose the Wi-Fi network "
    "AirTrack should use and where it lives. Setup stays entirely on this "
    "device.</p><section class=card><h2>" SETUP_ICON_WIFI "Setup hotspot</h2>"
    "<div class=join><span>SSID</span><code>";

static const char PAGE_MIDDLE[] =
    "</code><span>Password</span><code class=key>";

static const char PAGE_AFTER_PASSWORD[] =
    "</code><span>Address</span><code>";

static const char PAGE_FORM_START[] =
    "</code></div></section><form method=post action=\"";

static const char PAGE_FORM_BEFORE_TOKEN[] =
    "\" accept-charset=UTF-8 autocomplete=off>"
    "<input type=hidden name=csrf_token value=\"";

static const char PAGE_FORM_BEFORE_NETWORKS[] =
    "\"><section class=card><h2>Wi-Fi network</h2>"
    "<label for=nearby>Nearby networks</label>"
    "<select id=nearby aria-describedby=network-hint>";

static const char PAGE_FORM_AFTER_NETWORKS[] =
    "</select><p id=network-hint class=hint>Security and signal strength are "
    "shown beside each network. Choose one, or type a hidden network below.</p>"
    "<label for=ssid>Network name <span class=manual>(editable)</span></label>"
    "<input id=ssid name=ssid type=text maxlength=32 autocomplete=off "
    "required value=\"";

static const char PAGE_FORM_AFTER_SSID[] =
    "\"><label for=password>Password</label>"
    "<input id=password name=password type=password maxlength=63 "
    "autocomplete=off>"
    "<p class=hint>Leave blank only for an open network; protected networks "
    "need 8 to 63 characters. A saved password is never shown here, so re-enter "
    "it when keeping the same network.</p></section>"
    "<section class=card><h2>Tracking location</h2>"
    "<p class=hint style=\"margin:0 0 4px\">The fixed position of this display, "
    "used only to request nearby aircraft from adsb.fi. You can refine it later "
    "from the dashboard, which can also read your phone's location.</p>"
    "<div class=two><div><label for=latitude>Latitude</label>"
    "<input id=latitude name=latitude type=number step=any min=-90 max=90 "
    "placeholder=37.6213 required";

static const char PAGE_FORM_AFTER_LATITUDE[] =
    "></div><div><label for=longitude>Longitude</label>"
    "<input id=longitude name=longitude type=number step=any min=-180 max=180 "
    "placeholder=-122.3790 required";

static const char PAGE_FORM_AFTER_LONGITUDE[] =
    "></div></div><label for=radius>Search radius (nautical miles)</label>"
    "<input id=radius name=radius type=number min=1 max=250 required value=";

/* Advanced options are rendered with printf-style placeholders resolved by
 * send_options_section(); every value is a bounded integer or enum. */
static const char PAGE_FORM_OPTIONS_HEAD[] =
    "><details class=opts><summary>Display &amp; feed options</summary>"
    "<label for=units>Distance units</label><select id=units name=units>";

static const char PAGE_FORM_OPTIONS_TAIL[] =
    "<p class=hint>Ground aircraft are excluded by default so parked traffic "
    "at a nearby airport does not hide the closest airborne target. SD "
    "logging writes NDJSON to /airtrack/logs when a FAT32 card is present. "
    "Everything here can be changed later from the dashboard.</p>"
    "</details></section><button type=submit>" SETUP_ICON_SAVE
    "Save and connect</button></form>"
    "<script>const network=document.getElementById('nearby'),ssid=document."
    "getElementById('ssid');network.onchange=function(){if(network.value){ssid."
    "value=network.value;document.getElementById('password').focus()}};</script>"
    "<footer>Data provided by adsb.fi &middot; Not for navigation or "
    "collision avoidance.</footer></main></body></html>";

static const char RESULT_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AirTrack setup</title><style>"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;"
    "font-family:system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;background:#f3f5f8;color:#12213a}"
    "main{width:min(100%,460px);padding:24px;border:1px solid #e3e8ef;"
    "border-radius:12px;background:#fff;box-shadow:0 1px 2px #0d2a4d0f}h1{margin-top:0;color:#0d2a4d}"
    "p{line-height:1.55;color:#5d6b80}a{color:#1e88e5}small{color:#5d6b80}"
    "</style></head><body><main>";

static esp_err_t set_response_headers(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    }
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(
            request, "Content-Security-Policy",
            "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
            "script-src 'sha256-p03dkctonOn5oiXvbj25/fkDhwK/+K/B6mlySHggnc4='; "
            "base-uri 'none'; frame-ancestors 'none'");
    }
    return result;
}

static bool request_has_canonical_host(httpd_req_t *request)
{
    const size_t address_length = strlen(s_web.ap_ip_address);
    const size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    if (host_length != address_length && host_length != address_length + 3U) {
        return false;
    }

    char host[SETUP_WEB_CANONICAL_HOST_MAX_BYTES];
    if (host_length + 1U > sizeof(host) ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) !=
            ESP_OK ||
        memcmp(host, s_web.ap_ip_address, address_length) != 0) {
        return false;
    }
    return host[address_length] == '\0' ||
           strcmp(host + address_length, ":80") == 0;
}

static esp_err_t send_canonical_redirect(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_status(request, "302 Found");
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request, "Location", s_web.canonical_url);
    }
    if (result == ESP_OK) {
        result = set_response_headers(request);
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
        const char *escaped = NULL;
        switch (*cursor) {
        case '&':
            escaped = "&amp;";
            break;
        case '<':
            escaped = "&lt;";
            break;
        case '>':
            escaped = "&gt;";
            break;
        case '\"':
            escaped = "&quot;";
            break;
        case '\'':
            escaped = "&#39;";
            break;
        default:
            break;
        }

        const char literal[2] = {(char)*cursor, '\0'};
        const char *fragment = escaped != NULL ? escaped : literal;
        const size_t fragment_length = strlen(fragment);
        if (used + fragment_length > sizeof(buffer)) {
            const esp_err_t result = flush_escape_buffer(request, buffer, &used);
            if (result != ESP_OK) {
                return result;
            }
        }
        memcpy(buffer + used, fragment, fragment_length);
        used += fragment_length;
    }

    return flush_escape_buffer(request, buffer, &used);
}

static const char *signal_description(int8_t rssi)
{
    if (rssi >= -55) {
        return "Excellent";
    }
    if (rssi >= -67) {
        return "Good";
    }
    if (rssi >= -75) {
        return "Fair";
    }
    return "Weak";
}

static esp_err_t send_network_option(httpd_req_t *request,
                                     const setup_web_network_t *network)
{
    esp_err_t result = send_html_chunk(request, "<option value=\"");
    if (result == ESP_OK) {
        result = send_html_escaped(request, network->ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "\">");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, network->ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request,
                                 network->secured ? " &middot; Locked &middot; "
                                                  : " &middot; Open &middot; ");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, signal_description(network->rssi));
    }
    if (result == ESP_OK) {
        char strength[24];
        const int length = snprintf(strength, sizeof(strength),
                                    " (%d dBm)</option>", (int)network->rssi);
        if (length < 0 || (size_t)length >= sizeof(strength)) {
            return ESP_FAIL;
        }
        result = httpd_resp_send_chunk(request, strength, (size_t)length);
    }
    return result;
}

static esp_err_t send_select_option(httpd_req_t *request, const char *value,
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
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</option>");
    }
    return result;
}

static esp_err_t send_options_section(httpd_req_t *request)
{
    const airtrack_settings_t *settings = &s_web.current_settings;
    esp_err_t result = send_html_chunk(request, PAGE_FORM_OPTIONS_HEAD);
    if (result == ESP_OK) {
        result = send_select_option(request, "nm", "Nautical miles",
            settings->distance_unit == AIRTRACK_DISTANCE_NM);
    }
    if (result == ESP_OK) {
        result = send_select_option(request, "km", "Kilometres",
            settings->distance_unit == AIRTRACK_DISTANCE_KM);
    }
    if (result == ESP_OK) {
        result = send_select_option(request, "mi", "Statute miles",
            settings->distance_unit == AIRTRACK_DISTANCE_MI);
    }
    if (result == ESP_OK) {
        char numeric[512];
        const int length = snprintf(numeric, sizeof(numeric),
            "</select><div class=row><div><label for=poll>Refresh interval (s)"
            "</label><input id=poll name=poll type=number min=2 max=300 "
            "value=%u></div><div><label for=brightness>Brightness (%%)</label>"
            "<input id=brightness name=brightness type=number min=0 max=50 "
            "value=%u></div></div>"
            "<label class=check><input type=checkbox name=ground value=1%s>"
            "Include aircraft on the ground</label>"
            "<label for=logging>SD card logging</label>"
            "<select id=logging name=logging>",
            (unsigned)settings->poll_interval_s,
            (unsigned)settings->brightness_percent,
            settings->include_ground ? " checked" : "");
        result = (length < 0 || (size_t)length >= sizeof(numeric))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, numeric, (size_t)length);
    }
    if (result == ESP_OK) {
        result = send_select_option(request, "0", "Off",
            settings->logging_mode == AIRTRACK_LOGGING_OFF);
    }
    if (result == ESP_OK) {
        result = send_select_option(request, "1", "Target changes",
            settings->logging_mode == AIRTRACK_LOGGING_TARGET_CHANGES);
    }
    if (result == ESP_OK) {
        result = send_select_option(request, "2", "Target changes + heartbeat",
            settings->logging_mode == AIRTRACK_LOGGING_PERIODIC);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</select>");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_OPTIONS_TAIL);
    }
    return result;
}

static esp_err_t send_location_fields(httpd_req_t *request)
{
    const airtrack_settings_t *settings = &s_web.current_settings;
    char text[48];
    esp_err_t result = ESP_OK;
    if (settings->location_configured) {
        const int length = snprintf(text, sizeof(text), " value=%.6f",
            (double)settings->latitude_e7 / 10000000.0);
        result = (length < 0 || (size_t)length >= sizeof(text))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, text, (size_t)length);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_AFTER_LATITUDE);
    }
    if (result == ESP_OK && settings->location_configured) {
        const int length = snprintf(text, sizeof(text), " value=%.6f",
            (double)settings->longitude_e7 / 10000000.0);
        result = (length < 0 || (size_t)length >= sizeof(text))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, text, (size_t)length);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_AFTER_LONGITUDE);
    }
    if (result == ESP_OK) {
        const int length = snprintf(text, sizeof(text), "%u",
                                    (unsigned)settings->radius_nm);
        result = (length < 0 || (size_t)length >= sizeof(text))
                     ? ESP_ERR_INVALID_SIZE
                     : httpd_resp_send_chunk(request, text, (size_t)length);
    }
    return result;
}

static esp_err_t setup_page_handler(httpd_req_t *request)
{
    if (!request_has_canonical_host(request)) {
        return send_canonical_redirect(request);
    }

    esp_err_t result = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (result == ESP_OK) {
        result = set_response_headers(request);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_HEAD);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, SETUP_CSS);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_CSS);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.ap_ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_MIDDLE);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.ap_password);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_AFTER_PASSWORD);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.ap_ip_address);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_START);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.canonical_action);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_BEFORE_TOKEN);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.csrf_token);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_BEFORE_NETWORKS);
    }
    if (result == ESP_OK && s_web.nearby_network_count == 0U) {
        result = send_html_chunk(
            request,
            "<option value=\"\" selected disabled>No nearby networks found</option>");
    }
    if (result == ESP_OK && s_web.nearby_network_count > 0U) {
        result = send_html_chunk(
            request,
            "<option value=\"\" selected>Select a nearby network&hellip;</option>");
    }
    for (size_t index = 0U;
         result == ESP_OK && index < s_web.nearby_network_count; ++index) {
        result = send_network_option(request, &s_web.nearby_networks[index]);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_AFTER_NETWORKS);
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, s_web.current_ssid);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, PAGE_FORM_AFTER_SSID);
    }
    if (result == ESP_OK) {
        result = send_location_fields(request);
    }
    if (result == ESP_OK) {
        result = send_options_section(request);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static esp_err_t send_result_page(httpd_req_t *request, const char *status,
                                  const char *title, const char *message,
                                  bool retry_link)
{
    esp_err_t result = httpd_resp_set_status(request, status);
    if (result == ESP_OK) {
        result = httpd_resp_set_type(request, "text/html; charset=utf-8");
    }
    if (result == ESP_OK) {
        result = set_response_headers(request);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, RESULT_HEAD);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "<h1>");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, title);
    }
    if (result == ESP_OK) {
        result = send_html_chunk(request, "</h1><p>");
    }
    if (result == ESP_OK) {
        result = send_html_escaped(request, message);
    }
    if (result == ESP_OK && retry_link) {
        result = send_html_chunk(request, "</p><p><a href=/>Return to setup</a>");
    }
    if (result == ESP_OK) {
        result = send_html_chunk(
            request, "</p><small>Data: adsb.fi</small></main></body></html>");
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return result;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static esp_err_t decode_form_component(const char *encoded,
                                       size_t encoded_length, char *decoded,
                                       size_t decoded_capacity)
{
    size_t output_length = 0U;
    for (size_t index = 0U; index < encoded_length; ++index) {
        unsigned char value = (unsigned char)encoded[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (index + 2U >= encoded_length) {
                return ESP_ERR_INVALID_ARG;
            }
            const int high = hex_value(encoded[index + 1U]);
            const int low = hex_value(encoded[index + 2U]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            value = (unsigned char)((high << 4) | low);
            index += 2U;
        }

        if (value < 0x20U || value == 0x7fU) {
            return ESP_ERR_INVALID_ARG;
        }
        if (output_length + 1U >= decoded_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
        decoded[output_length++] = (char)value;
    }

    decoded[output_length] = '\0';
    return ESP_OK;
}

static bool valid_utf8(const unsigned char *text, size_t length);

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

static esp_err_t parse_wifi_form(const char *body, size_t body_length,
                                 setup_web_submission_t *submission,
                                 char *csrf_token, size_t csrf_token_capacity)
{
    if (body == NULL || body_length == 0U || body[body_length - 1U] == '&') {
        return ESP_ERR_INVALID_ARG;
    }

    bool have_ssid = false;
    bool have_password = false;
    bool have_csrf_token = false;
    bool have_latitude = false;
    bool have_longitude = false;
    bool have_radius = false;
    bool have_units = false;
    bool have_poll = false;
    bool have_brightness = false;
    bool have_ground = false;
    bool have_logging = false;
    char latitude[24] = {0};
    char longitude[24] = {0};
    char radius[8] = {0};
    char units[4] = {0};
    char poll[8] = {0};
    char brightness[8] = {0};
    char ground[4] = {0};
    char logging[4] = {0};
    char *ssid = submission->ssid;
    char *password = submission->password;
    const size_t ssid_capacity = sizeof(submission->ssid);
    const size_t password_capacity = sizeof(submission->password);
    size_t offset = 0U;
    while (offset < body_length) {
        const char *pair = body + offset;
        const char *ampersand = memchr(pair, '&', body_length - offset);
        const size_t pair_length = ampersand != NULL
                                       ? (size_t)(ampersand - pair)
                                       : body_length - offset;
        if (pair_length == 0U) {
            return ESP_ERR_INVALID_ARG;
        }

        const char *equals = memchr(pair, '=', pair_length);
        if (equals == NULL || equals == pair) {
            return ESP_ERR_INVALID_ARG;
        }

        char key[16];
        esp_err_t result = decode_form_component(
            pair, (size_t)(equals - pair), key, sizeof(key));
        if (result != ESP_OK) {
            return result;
        }

        const char *value = equals + 1;
        const size_t value_length =
            pair_length - (size_t)(value - pair);
        if (strcmp(key, "ssid") == 0 && !have_ssid) {
            result = decode_form_component(value, value_length, ssid,
                                           ssid_capacity);
            have_ssid = result == ESP_OK;
        } else if (strcmp(key, "password") == 0 && !have_password) {
            result = decode_form_component(value, value_length, password,
                                           password_capacity);
            have_password = result == ESP_OK;
        } else if (strcmp(key, "csrf_token") == 0 && !have_csrf_token) {
            result = decode_form_component(value, value_length, csrf_token,
                                           csrf_token_capacity);
            have_csrf_token = result == ESP_OK;
        } else if (strcmp(key, "latitude") == 0 && !have_latitude) {
            result = decode_form_component(value, value_length, latitude,
                                           sizeof(latitude));
            have_latitude = result == ESP_OK;
        } else if (strcmp(key, "longitude") == 0 && !have_longitude) {
            result = decode_form_component(value, value_length, longitude,
                                           sizeof(longitude));
            have_longitude = result == ESP_OK;
        } else if (strcmp(key, "radius") == 0 && !have_radius) {
            result = decode_form_component(value, value_length, radius,
                                           sizeof(radius));
            have_radius = result == ESP_OK;
        } else if (strcmp(key, "units") == 0 && !have_units) {
            result = decode_form_component(value, value_length, units,
                                           sizeof(units));
            have_units = result == ESP_OK;
        } else if (strcmp(key, "poll") == 0 && !have_poll) {
            result = decode_form_component(value, value_length, poll,
                                           sizeof(poll));
            have_poll = result == ESP_OK;
        } else if (strcmp(key, "brightness") == 0 && !have_brightness) {
            result = decode_form_component(value, value_length, brightness,
                                           sizeof(brightness));
            have_brightness = result == ESP_OK;
        } else if (strcmp(key, "ground") == 0 && !have_ground) {
            result = decode_form_component(value, value_length, ground,
                                           sizeof(ground));
            have_ground = result == ESP_OK;
        } else if (strcmp(key, "logging") == 0 && !have_logging) {
            result = decode_form_component(value, value_length, logging,
                                           sizeof(logging));
            have_logging = result == ESP_OK;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        if (result != ESP_OK) {
            return result;
        }

        offset += pair_length + (ampersand != NULL ? 1U : 0U);
    }

    if (!have_ssid || !have_password || !have_csrf_token || !have_latitude ||
        !have_longitude || !have_radius ||
        !valid_csrf_token_text(csrf_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0U || ssid_length > SETUP_WEB_SSID_MAX_BYTES ||
        password_length > SETUP_WEB_PASSWORD_MAX_BYTES ||
        (password_length > 0U && password_length < 8U) ||
        !valid_utf8((const unsigned char *)ssid, ssid_length) ||
        !valid_utf8((const unsigned char *)password, password_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    char *latitude_end = NULL;
    char *longitude_end = NULL;
    const double latitude_value = strtod(latitude, &latitude_end);
    const double longitude_value = strtod(longitude, &longitude_end);
    unsigned long radius_value = 0UL;
    if (errno != 0 || latitude_end == latitude || *latitude_end != '\0' ||
        longitude_end == longitude || *longitude_end != '\0' ||
        !parse_bounded_ulong(radius, 1UL, 250UL, &radius_value) ||
        !isfinite(latitude_value) || !isfinite(longitude_value) ||
        latitude_value < -90.0 || latitude_value > 90.0 ||
        longitude_value < -180.0 || longitude_value > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }
    airtrack_settings_t *settings = &submission->settings;
    settings->location_configured = true;
    settings->latitude_e7 = (int32_t)llround(latitude_value * 10000000.0);
    settings->longitude_e7 = (int32_t)llround(longitude_value * 10000000.0);
    settings->radius_nm = (uint16_t)radius_value;

    /* Optional advanced fields: absent keys keep the current values.  A
     * checkbox is only posted when checked, so its presence in the form is
     * signalled by the units field, which is always posted with it. */
    if (have_units) {
        if (strcmp(units, "nm") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_NM;
        } else if (strcmp(units, "km") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_KM;
        } else if (strcmp(units, "mi") == 0) {
            settings->distance_unit = AIRTRACK_DISTANCE_MI;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        settings->include_ground = have_ground && strcmp(ground, "1") == 0;
    }
    unsigned long numeric = 0UL;
    if (have_poll) {
        if (!parse_bounded_ulong(poll, 2UL, 300UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->poll_interval_s = (uint16_t)numeric;
    }
    if (have_brightness) {
        if (!parse_bounded_ulong(brightness, 0UL, 50UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->brightness_percent = (uint8_t)numeric;
    }
    if (have_logging) {
        if (!parse_bounded_ulong(logging, 0UL, 2UL, &numeric)) {
            return ESP_ERR_INVALID_ARG;
        }
        settings->logging_mode = (airtrack_logging_mode_t)numeric;
    }
    return airtrack_settings_validate(settings);
}

static bool has_form_content_type(httpd_req_t *request)
{
    const size_t value_length =
        httpd_req_get_hdr_value_len(request, "Content-Type");
    if (value_length == 0U ||
        value_length > SETUP_WEB_CONTENT_TYPE_MAX_BYTES) {
        return false;
    }

    char content_type[SETUP_WEB_CONTENT_TYPE_MAX_BYTES + 1U];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK) {
        return false;
    }

    char *parameters = strchr(content_type, ';');
    if (parameters != NULL) {
        *parameters = '\0';
    }
    size_t media_length = strlen(content_type);
    while (media_length > 0U &&
           isspace((unsigned char)content_type[media_length - 1U])) {
        content_type[--media_length] = '\0';
    }
    char *media_type = content_type;
    while (isspace((unsigned char)*media_type)) {
        ++media_type;
    }
    return strcasecmp(media_type, "application/x-www-form-urlencoded") == 0;
}

static esp_err_t wifi_form_handler(httpd_req_t *request)
{
    if (!request_has_canonical_host(request)) {
        return send_result_page(
            request, "403 Forbidden", "Invalid setup address",
            "Reload setup using the local address shown on AirTrack.", false);
    }
    if (!has_form_content_type(request)) {
        return send_result_page(
            request, "415 Unsupported Media Type", "Unsupported request",
            "Submit the AirTrack setup form from this page.", true);
    }
    if (request->content_len <= 0 ||
        (size_t)request->content_len > SETUP_WEB_MAX_FORM_BYTES) {
        return send_result_page(request, "413 Payload Too Large",
                                "Request too large",
                                "Wi-Fi settings exceeded the allowed size.",
                                true);
    }

    char body[SETUP_WEB_MAX_FORM_BYTES + 1U] = {0};
    size_t received = 0U;
    unsigned int timeout_count = 0U;
    while (received < (size_t)request->content_len) {
        const int chunk = httpd_req_recv(
            request, body + received, (size_t)request->content_len - received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT && timeout_count++ < 2U) {
            continue;
        }
        if (chunk <= 0) {
            clear_sensitive_buffer(body, sizeof(body));
            return send_result_page(request, "408 Request Timeout",
                                    "Request timed out",
                                    "AirTrack did not receive the complete form.",
                                    true);
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    setup_web_submission_t submission;
    memset(&submission, 0, sizeof(submission));
    submission.settings = s_web.current_settings;
    char csrf_token[SETUP_WEB_CSRF_TOKEN_BYTES + 1U] = {0};
    const esp_err_t parse_result =
        parse_wifi_form(body, received, &submission, csrf_token,
                        sizeof(csrf_token));
    if (parse_result != ESP_OK) {
        clear_sensitive_buffer(csrf_token, sizeof(csrf_token));
        clear_sensitive_buffer(&submission, sizeof(submission));
        clear_sensitive_buffer(body, sizeof(body));
        return send_result_page(
            request, "400 Bad Request", "Check these settings",
            "Check the Wi-Fi credentials, coordinates, and 1-250 NM radius.",
            true);
    }

    if (!csrf_token_matches(csrf_token)) {
        clear_sensitive_buffer(csrf_token, sizeof(csrf_token));
        clear_sensitive_buffer(&submission, sizeof(submission));
        clear_sensitive_buffer(body, sizeof(body));
        return send_result_page(
            request, "403 Forbidden", "Setup session expired",
            "Reload the setup page before saving Wi-Fi settings.", true);
    }

    const esp_err_t save_result =
        s_web.save_config(&submission, s_web.user_context);
    clear_sensitive_buffer(csrf_token, sizeof(csrf_token));
    clear_sensitive_buffer(&submission, sizeof(submission));
    clear_sensitive_buffer(body, sizeof(body));
    if (save_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi settings callback returned %s",
                 esp_err_to_name(save_result));
        return send_result_page(
            request, "500 Internal Server Error", "Could not save settings",
            "AirTrack kept its previous settings. Please try again.", true);
    }

    return send_result_page(
        request, "200 OK", "Settings saved",
        "AirTrack is restarting its network connection. This setup page may "
        "disconnect while it joins your Wi-Fi.",
        false);
}

static const httpd_uri_t URI_ROOT = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_ANDROID = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_ANDROID_ALT = {
    .uri = "/gen_204",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_APPLE = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_APPLE_LIBRARY = {
    .uri = "/library/test/success.html",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_CANONICAL = {
    .uri = "/canonical.html",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_WINDOWS_NCSI = {
    .uri = "/ncsi.txt",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_WINDOWS_CONNECT = {
    .uri = "/connecttest.txt",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

static const httpd_uri_t URI_WIFI_CONFIG = {
    .uri = "/api/v1/config/wifi",
    .method = HTTP_POST,
    .handler = wifi_form_handler,
};

static const httpd_uri_t URI_GET_FALLBACK = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = setup_page_handler,
};

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
                /* Exclude Unicode C1 controls U+0080 through U+009F. */
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

static bool valid_display_value(const char *value, size_t max_length,
                                bool allow_empty)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, max_length + 1U);
    if (length > max_length || (!allow_empty && length == 0U)) {
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

static void copy_config_value(char *destination, size_t capacity,
                              const char *source)
{
    const size_t length = strnlen(source, capacity - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool valid_nearby_network_array(const setup_web_config_t *config)
{
    if (config->nearby_network_count > SETUP_WEB_MAX_NETWORKS ||
        (config->nearby_network_count > 0U &&
         config->nearby_networks == NULL)) {
        return false;
    }
    return true;
}

esp_err_t setup_web_start(const setup_web_config_t *config)
{
    if (config == NULL || config->save_config == NULL ||
        config->current_settings == NULL ||
        airtrack_settings_validate(config->current_settings) != ESP_OK ||
        (config->current_ssid != NULL &&
         !valid_display_value(config->current_ssid, SETUP_WEB_SSID_MAX_BYTES,
                              true)) ||
        !valid_display_value(config->ap_ssid, SETUP_WEB_SSID_MAX_BYTES, false) ||
        !valid_display_value(config->ap_password,
                             SETUP_WEB_PASSWORD_MAX_BYTES, false) ||
        !valid_display_value(config->ap_ip_address,
                             SETUP_WEB_IPV4_TEXT_MAX_BYTES, false) ||
        !valid_nearby_network_array(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_web.server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_web, 0, sizeof(s_web));
    copy_config_value(s_web.ap_ssid, sizeof(s_web.ap_ssid), config->ap_ssid);
    copy_config_value(s_web.ap_password, sizeof(s_web.ap_password),
                      config->ap_password);
    copy_config_value(s_web.ap_ip_address, sizeof(s_web.ap_ip_address),
                      config->ap_ip_address);
    const int canonical_url_length =
        snprintf(s_web.canonical_url, sizeof(s_web.canonical_url),
                 "http://%s/", s_web.ap_ip_address);
    const int canonical_action_length =
        snprintf(s_web.canonical_action, sizeof(s_web.canonical_action),
                 "http://%s/api/v1/config/wifi", s_web.ap_ip_address);
    if (canonical_url_length < 0 ||
        (size_t)canonical_url_length >= sizeof(s_web.canonical_url) ||
        canonical_action_length < 0 ||
        (size_t)canonical_action_length >= sizeof(s_web.canonical_action)) {
        memset(&s_web, 0, sizeof(s_web));
        return ESP_ERR_INVALID_SIZE;
    }
    make_csrf_token(s_web.csrf_token);
    for (size_t index = 0U; index < config->nearby_network_count; ++index) {
        if (!valid_display_value(config->nearby_networks[index].ssid,
                                 SETUP_WEB_SSID_MAX_BYTES, false)) {
            continue;
        }
        setup_web_network_t *network =
            &s_web.nearby_networks[s_web.nearby_network_count];
        copy_config_value(network->ssid, sizeof(network->ssid),
                          config->nearby_networks[index].ssid);
        network->rssi = config->nearby_networks[index].rssi;
        network->secured = config->nearby_networks[index].secured;
        ++s_web.nearby_network_count;
    }
    s_web.current_settings = *config->current_settings;
    if (config->current_ssid != NULL) {
        copy_config_value(s_web.current_ssid, sizeof(s_web.current_ssid),
                          config->current_ssid);
    }
    s_web.save_config = config->save_config;
    s_web.user_context = config->user_context;

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 7168U;
    server_config.max_open_sockets = 3U;
    server_config.max_uri_handlers = 12U;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 5U;
    server_config.send_wait_timeout = 5U;

    esp_err_t result = httpd_start(&s_web.server, &server_config);
    if (result != ESP_OK) {
        memset(&s_web, 0, sizeof(s_web));
        return result;
    }

    const httpd_uri_t *handlers[] = {
        &URI_ROOT,
        &URI_ANDROID,
        &URI_ANDROID_ALT,
        &URI_APPLE,
        &URI_APPLE_LIBRARY,
        &URI_CANONICAL,
        &URI_WINDOWS_NCSI,
        &URI_WINDOWS_CONNECT,
        &URI_WIFI_CONFIG,
        &URI_GET_FALLBACK,
    };
    for (size_t index = 0U; index < sizeof(handlers) / sizeof(handlers[0]);
         ++index) {
        result = httpd_register_uri_handler(s_web.server, handlers[index]);
        if (result != ESP_OK) {
            httpd_stop(s_web.server);
            memset(&s_web, 0, sizeof(s_web));
            return result;
        }
    }

    ESP_LOGI(TAG, "Local setup server started");
    return ESP_OK;
}

esp_err_t setup_web_stop(void)
{
    if (s_web.server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_handle_t server = s_web.server;
    const esp_err_t result = httpd_stop(server);
    if (result == ESP_OK) {
        memset(&s_web, 0, sizeof(s_web));
    }
    return result;
}

bool setup_web_is_running(void)
{
    return s_web.server != NULL;
}
