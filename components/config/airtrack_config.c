#include "airtrack_config.h"

#include <stdint.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "airtrack"
#define KEY_WIFI_CONFIGURED "wifi_cfg"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASSWORD "wifi_pass"
#define KEY_AP_SSID "ap_ssid"
#define KEY_AP_PASSWORD "ap_pass"
#define KEY_SETTINGS_A "trk_a"
#define KEY_SETTINGS_B "trk_b"

#define AP_PREFIX "AirTrack-"
#define SETTINGS_MAGIC 0x4b525441UL /* "ATRK" in little-endian storage. */
#define SETTINGS_SCHEMA 1U
#define SETTINGS_WIRE_BYTES 80U

enum {
    WIRE_MAGIC = 0,
    WIRE_SCHEMA = 4,
    WIRE_LENGTH = 6,
    WIRE_GENERATION = 8,
    WIRE_CRC = 16,
    WIRE_LOCATION_CONFIGURED = 20,
    WIRE_LATITUDE_E7 = 21,
    WIRE_LONGITUDE_E7 = 25,
    WIRE_RADIUS_NM = 29,
    WIRE_POLL_INTERVAL_S = 31,
    WIRE_MAX_POSITION_AGE_S = 33,
    WIRE_INCLUDE_GROUND = 35,
    WIRE_DISTANCE_UNIT = 36,
    WIRE_BRIGHTNESS = 37,
    WIRE_LOGGING_MODE = 38,
    WIRE_LOG_HEARTBEAT_S = 39,
    WIRE_RETENTION_DAYS = 41,
    WIRE_RETENTION_MIB = 43,
    WIRE_HOSTNAME_LENGTH = 45,
    WIRE_HOSTNAME = 46,
};

static const char AP_PASSWORD_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
_Static_assert(sizeof(AP_PASSWORD_ALPHABET) - 1U == 32U,
               "AP password alphabet must contain 32 symbols");

static bool s_initialized;

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        out[shift / 8U] = (uint8_t)(value >> shift);
    }
}

static void put_u64(uint8_t *out, uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        out[shift / 8U] = (uint8_t)(value >> shift);
    }
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8U);
}

static uint32_t get_u32(const uint8_t *in)
{
    uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= (uint32_t)in[shift / 8U] << shift;
    }
    return value;
}

static uint64_t get_u64(const uint8_t *in)
{
    uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= (uint64_t)in[shift / 8U] << shift;
    }
    return value;
}

void airtrack_settings_defaults(airtrack_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = (airtrack_settings_t) {
        .radius_nm = 25U,
        .poll_interval_s = 5U,
        .max_position_age_s = 15U,
        .distance_unit = AIRTRACK_DISTANCE_NM,
        .brightness_percent = 40U,
        .logging_mode = AIRTRACK_LOGGING_OFF,
        .log_heartbeat_s = 60U,
        .retention_days = 30U,
        .retention_mib = 64U,
    };
    memcpy(out->hostname, "airtrack", sizeof("airtrack"));
}

static bool hostname_valid(const char *hostname)
{
    if (hostname == NULL) {
        return false;
    }
    const size_t length = strnlen(hostname, AIRTRACK_HOSTNAME_MAX_LENGTH + 1U);
    if (length == 0U || length > AIRTRACK_HOSTNAME_MAX_LENGTH ||
        hostname[0] == '-' || hostname[length - 1U] == '-') {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char byte = hostname[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t airtrack_settings_validate(const airtrack_settings_t *settings)
{
    if (settings == NULL ||
        settings->latitude_e7 < -900000000 ||
        settings->latitude_e7 > 900000000 ||
        settings->longitude_e7 < -1800000000 ||
        settings->longitude_e7 > 1800000000 ||
        settings->radius_nm < 1U || settings->radius_nm > 250U ||
        settings->poll_interval_s < 2U || settings->poll_interval_s > 300U ||
        settings->max_position_age_s < 5U ||
        settings->max_position_age_s > 120U ||
        settings->distance_unit > AIRTRACK_DISTANCE_MI ||
        settings->brightness_percent > 50U ||
        settings->logging_mode > AIRTRACK_LOGGING_PERIODIC ||
        settings->log_heartbeat_s < 30U ||
        settings->log_heartbeat_s > 3600U ||
        settings->retention_days < 1U || settings->retention_days > 365U ||
        settings->retention_mib < 8U || settings->retention_mib > 4096U ||
        !hostname_valid(settings->hostname)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void encode_settings(const airtrack_settings_t *settings,
                            uint8_t wire[SETTINGS_WIRE_BYTES])
{
    memset(wire, 0, SETTINGS_WIRE_BYTES);
    put_u32(wire + WIRE_MAGIC, SETTINGS_MAGIC);
    put_u16(wire + WIRE_SCHEMA, SETTINGS_SCHEMA);
    put_u16(wire + WIRE_LENGTH, SETTINGS_WIRE_BYTES);
    put_u64(wire + WIRE_GENERATION, settings->generation);
    wire[WIRE_LOCATION_CONFIGURED] = settings->location_configured ? 1U : 0U;
    put_u32(wire + WIRE_LATITUDE_E7, (uint32_t)settings->latitude_e7);
    put_u32(wire + WIRE_LONGITUDE_E7, (uint32_t)settings->longitude_e7);
    put_u16(wire + WIRE_RADIUS_NM, settings->radius_nm);
    put_u16(wire + WIRE_POLL_INTERVAL_S, settings->poll_interval_s);
    put_u16(wire + WIRE_MAX_POSITION_AGE_S, settings->max_position_age_s);
    wire[WIRE_INCLUDE_GROUND] = settings->include_ground ? 1U : 0U;
    wire[WIRE_DISTANCE_UNIT] = (uint8_t)settings->distance_unit;
    wire[WIRE_BRIGHTNESS] = settings->brightness_percent;
    wire[WIRE_LOGGING_MODE] = (uint8_t)settings->logging_mode;
    put_u16(wire + WIRE_LOG_HEARTBEAT_S, settings->log_heartbeat_s);
    put_u16(wire + WIRE_RETENTION_DAYS, settings->retention_days);
    put_u16(wire + WIRE_RETENTION_MIB, settings->retention_mib);
    const size_t hostname_length = strlen(settings->hostname);
    wire[WIRE_HOSTNAME_LENGTH] = (uint8_t)hostname_length;
    memcpy(wire + WIRE_HOSTNAME, settings->hostname, hostname_length);
    put_u32(wire + WIRE_CRC, 0U);
    put_u32(wire + WIRE_CRC,
            esp_crc32_le(UINT32_MAX, wire, SETTINGS_WIRE_BYTES));
}

static bool decode_settings(const uint8_t *wire, size_t length,
                            airtrack_settings_t *out)
{
    if (wire == NULL || out == NULL || length != SETTINGS_WIRE_BYTES ||
        get_u32(wire + WIRE_MAGIC) != SETTINGS_MAGIC ||
        get_u16(wire + WIRE_SCHEMA) != SETTINGS_SCHEMA ||
        get_u16(wire + WIRE_LENGTH) != SETTINGS_WIRE_BYTES) {
        return false;
    }
    uint8_t checked[SETTINGS_WIRE_BYTES];
    memcpy(checked, wire, sizeof(checked));
    const uint32_t stored_crc = get_u32(checked + WIRE_CRC);
    put_u32(checked + WIRE_CRC, 0U);
    if (esp_crc32_le(UINT32_MAX, checked, sizeof(checked)) != stored_crc) {
        return false;
    }
    const uint8_t hostname_length = wire[WIRE_HOSTNAME_LENGTH];
    if (hostname_length == 0U || hostname_length > AIRTRACK_HOSTNAME_MAX_LENGTH) {
        return false;
    }

    airtrack_settings_t decoded = {
        .generation = get_u64(wire + WIRE_GENERATION),
        .location_configured = wire[WIRE_LOCATION_CONFIGURED] == 1U,
        .latitude_e7 = (int32_t)get_u32(wire + WIRE_LATITUDE_E7),
        .longitude_e7 = (int32_t)get_u32(wire + WIRE_LONGITUDE_E7),
        .radius_nm = get_u16(wire + WIRE_RADIUS_NM),
        .poll_interval_s = get_u16(wire + WIRE_POLL_INTERVAL_S),
        .max_position_age_s = get_u16(wire + WIRE_MAX_POSITION_AGE_S),
        .include_ground = wire[WIRE_INCLUDE_GROUND] == 1U,
        .distance_unit = (airtrack_distance_unit_t)wire[WIRE_DISTANCE_UNIT],
        .brightness_percent = wire[WIRE_BRIGHTNESS],
        .logging_mode = (airtrack_logging_mode_t)wire[WIRE_LOGGING_MODE],
        .log_heartbeat_s = get_u16(wire + WIRE_LOG_HEARTBEAT_S),
        .retention_days = get_u16(wire + WIRE_RETENTION_DAYS),
        .retention_mib = get_u16(wire + WIRE_RETENTION_MIB),
    };
    memcpy(decoded.hostname, wire + WIRE_HOSTNAME, hostname_length);
    decoded.hostname[hostname_length] = '\0';
    if ((wire[WIRE_LOCATION_CONFIGURED] > 1U) ||
        (wire[WIRE_INCLUDE_GROUND] > 1U) ||
        airtrack_settings_validate(&decoded) != ESP_OK) {
        return false;
    }
    *out = decoded;
    return true;
}

static bool read_settings_slot(nvs_handle_t handle, const char *key,
                               airtrack_settings_t *out)
{
    uint8_t wire[SETTINGS_WIRE_BYTES];
    size_t length = sizeof(wire);
    return nvs_get_blob(handle, key, wire, &length) == ESP_OK &&
           decode_settings(wire, length, out);
}

static esp_err_t read_string(nvs_handle_t handle, const char *key, char *out, size_t capacity)
{
    size_t required = capacity;
    esp_err_t err = nvs_get_str(handle, key, out, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (required == 0 || required > capacity || out[required - 1] != '\0') {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool wifi_credentials_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return false;
    }

    const size_t ssid_length = strnlen(ssid, sizeof(((airtrack_config_t *)0)->wifi_ssid));
    const size_t password_length =
        strnlen(password, sizeof(((airtrack_config_t *)0)->wifi_password));

    return ssid_length >= 1 && ssid_length <= 32 &&
           (password_length == 0 || (password_length >= 8 && password_length <= 63));
}

static esp_err_t make_ap_ssid(char out[33])
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t mac[6];
    /* ESP32-C6's eFuse default is an 8-byte EUI-64.  Request the derived
     * six-byte Wi-Fi STA address explicitly so the buffer size and visible
     * suffix are stable across radio-capable ESP targets. */
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(out, AP_PREFIX, sizeof(AP_PREFIX) - 1);
    out[9] = hex[mac[4] >> 4];
    out[10] = hex[mac[4] & 0x0f];
    out[11] = hex[mac[5] >> 4];
    out[12] = hex[mac[5] & 0x0f];
    out[13] = '\0';
    return ESP_OK;
}

static bool ap_password_valid(const char *password)
{
    if (strnlen(password, AIRTRACK_AP_PASSWORD_LENGTH + 1U) !=
        AIRTRACK_AP_PASSWORD_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < AIRTRACK_AP_PASSWORD_LENGTH; ++i) {
        if (strchr(AP_PASSWORD_ALPHABET, password[i]) == NULL) {
            return false;
        }
    }
    return true;
}

static void make_ap_password(
    char out[AIRTRACK_AP_PASSWORD_LENGTH + 1U])
{
    uint8_t random_bytes[AIRTRACK_AP_PASSWORD_LENGTH];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < AIRTRACK_AP_PASSWORD_LENGTH; ++i) {
        out[i] = AP_PASSWORD_ALPHABET[random_bytes[i] & 0x1fU];
    }
    out[AIRTRACK_AP_PASSWORD_LENGTH] = '\0';
}

static esp_err_t ensure_ap_identity(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    bool changed = false;
    char expected_ssid[33] = {0};
    char stored_ssid[33] = {0};
    char stored_password[AIRTRACK_AP_PASSWORD_LENGTH + 1U] = {0};

    err = make_ap_ssid(expected_ssid);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    if (read_string(handle, KEY_AP_SSID, stored_ssid, sizeof(stored_ssid)) != ESP_OK ||
        strcmp(stored_ssid, expected_ssid) != 0) {
        err = nvs_set_str(handle, KEY_AP_SSID, expected_ssid);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
        changed = true;
    }

    if (read_string(handle, KEY_AP_PASSWORD, stored_password, sizeof(stored_password)) != ESP_OK ||
        !ap_password_valid(stored_password)) {
        make_ap_password(stored_password);
        err = nvs_set_str(handle, KEY_AP_PASSWORD, stored_password);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
        changed = true;
    }

    if (changed) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t airtrack_config_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_ap_identity();
    if (err == ESP_OK) {
        s_initialized = true;
    }
    return err;
}

esp_err_t airtrack_config_load(airtrack_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = airtrack_config_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    airtrack_config_t loaded = {0};
    err = read_string(handle, KEY_AP_SSID, loaded.ap_ssid, sizeof(loaded.ap_ssid));
    if (err == ESP_OK) {
        err = read_string(handle, KEY_AP_PASSWORD, loaded.ap_password,
                          sizeof(loaded.ap_password));
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    uint8_t configured = 0;
    err = nvs_get_u8(handle, KEY_WIFI_CONFIGURED, &configured);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    if (configured == 1) {
        const esp_err_t ssid_err =
            read_string(handle, KEY_WIFI_SSID, loaded.wifi_ssid, sizeof(loaded.wifi_ssid));
        const esp_err_t password_err = read_string(handle, KEY_WIFI_PASSWORD,
                                                   loaded.wifi_password,
                                                   sizeof(loaded.wifi_password));
        if (ssid_err == ESP_OK && password_err == ESP_OK &&
            wifi_credentials_valid(loaded.wifi_ssid, loaded.wifi_password)) {
            loaded.wifi_configured = true;
        } else {
            loaded.wifi_ssid[0] = '\0';
            loaded.wifi_password[0] = '\0';
        }
    }

    nvs_close(handle);
    *out = loaded;
    return ESP_OK;
}

esp_err_t airtrack_config_save_wifi(const char *ssid, const char *password)
{
    if (!wifi_credentials_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = airtrack_config_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_WIFI_CONFIGURED, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t airtrack_config_clear_wifi(void)
{
    esp_err_t err = airtrack_config_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, KEY_WIFI_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, KEY_WIFI_PASSWORD);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_WIFI_CONFIGURED, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t airtrack_config_load_settings(airtrack_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = airtrack_config_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    airtrack_settings_t a;
    airtrack_settings_t b;
    const bool have_a = read_settings_slot(handle, KEY_SETTINGS_A, &a);
    const bool have_b = read_settings_slot(handle, KEY_SETTINGS_B, &b);
    nvs_close(handle);

    if (!have_a && !have_b) {
        airtrack_settings_defaults(out);
    } else if (!have_b || (have_a && a.generation >= b.generation)) {
        *out = a;
    } else {
        *out = b;
    }
    return ESP_OK;
}

esp_err_t airtrack_config_save_settings(const airtrack_settings_t *settings)
{
    if (airtrack_settings_validate(settings) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = airtrack_config_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    airtrack_settings_t a;
    airtrack_settings_t b;
    const bool have_a = read_settings_slot(handle, KEY_SETTINGS_A, &a);
    const bool have_b = read_settings_slot(handle, KEY_SETTINGS_B, &b);
    uint64_t newest_generation = 0U;
    if (have_a && a.generation > newest_generation) {
        newest_generation = a.generation;
    }
    if (have_b && b.generation > newest_generation) {
        newest_generation = b.generation;
    }
    if (newest_generation == UINT64_MAX) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    airtrack_settings_t next = *settings;
    next.generation = newest_generation + 1U;
    uint8_t wire[SETTINGS_WIRE_BYTES];
    encode_settings(&next, wire);
    const char *target = (!have_a || (have_b && a.generation <= b.generation))
                             ? KEY_SETTINGS_A
                             : KEY_SETTINGS_B;
    err = nvs_set_blob(handle, target, wire, sizeof(wire));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
        airtrack_settings_t verified;
        if (!read_settings_slot(handle, target, &verified) ||
            verified.generation != next.generation) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    nvs_close(handle);
    return err;
}
