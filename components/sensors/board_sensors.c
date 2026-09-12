/*
 * Optional I2C sensors on the ESP32-C6-Touch-LCD-2.8.
 *
 *   PCF85063A  0x51  battery-backed RTC, used to hold wall-clock time across
 *                    a restart so the device is not stuck in TIME_SYNC until
 *                    SNTP answers, and re-armed once SNTP does answer.
 *   SHTC3      0x70  temperature and humidity, shown on the dashboard.
 *
 * Register maps follow the NXP PCF85063A and Sensirion SHTC3 datasheets; the
 * bus wiring follows Waveshare's board sources.  Neither device is required:
 * the board runs identically without them.
 */

#include "board_sensors.h"

#if BOARD_HAS_RTC || BOARD_HAS_ENVIRONMENT_SENSOR

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SENSORS_I2C_TIMEOUT_MS 100
#define SENSORS_I2C_SPEED_HZ 400000U

/* PCF85063A. */
#define PCF85063A_ADDRESS 0x51
#define PCF85063A_REG_CONTROL_1 0x00
#define PCF85063A_REG_SECONDS 0x04
/* Seconds bit 7: the oscillator stopped, so the held time is not trustworthy. */
#define PCF85063A_SECONDS_OS_MASK 0x80
#define PCF85063A_CONTROL_1_STOP 0x20
/* The chip stores a two-digit year against a fixed century. */
#define PCF85063A_EPOCH_YEAR 2000

/* SHTC3. */
#define SHTC3_ADDRESS 0x70
#define SHTC3_CMD_WAKE 0x3517
#define SHTC3_CMD_SLEEP 0xB098
#define SHTC3_CMD_READ_ID 0xEFC8
/* Temperature first, no clock stretching, normal power. */
#define SHTC3_CMD_MEASURE 0x7866
#define SHTC3_MEASURE_DELAY_MS 15U
#define SHTC3_ID_MASK 0x083FU
#define SHTC3_ID_VALUE 0x0807U

/* The dashboard polls every two seconds; the air does not change that fast. */
#define ENVIRONMENT_CACHE_MS 10000
/*
 * How long a sample stays usable when a later read fails.  The SHTC3
 * occasionally NACKs a measurement, and blanking the reading over one lost
 * sample makes the dashboard flicker; holding the previous value for a while
 * is both steadier and more honest than reporting nothing.
 */
#define ENVIRONMENT_STALE_MS 60000

static const char *TAG = "sensors";

static struct {
    bool initialized;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    i2c_master_dev_handle_t rtc;
    i2c_master_dev_handle_t environment;
    bool environment_valid;
    float temperature_c;
    float humidity_percent;
    int64_t environment_sampled_ms;
} s_sensors;

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t attach(uint16_t address, i2c_master_dev_handle_t *out)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = SENSORS_I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, out);
}

#if BOARD_HAS_RTC
/* ---------------------------------------------------------------- PCF85063A */

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_sensors.rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_sensors.rtc, &reg, 1, data, length,
                                       SENSORS_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *data, size_t length)
{
    if (s_sensors.rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[8];
    if (length + 1U > sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    payload[0] = reg;
    memcpy(&payload[1], data, length);
    return i2c_master_transmit(s_sensors.rtc, payload, length + 1U,
                               SENSORS_I2C_TIMEOUT_MS);
}

/*
 * Days from 1970-01-01 to the given civil date, for years 1..9999 (Howard
 * Hinnant's days_from_civil).  The era arithmetic handles leap years,
 * including the 400-year rule, without a lookup table.
 */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    /* March-based year, so a leap day lands at the end and needs no case. */
    const unsigned shifted_month = month > 2U ? month - 3U : month + 9U;
    if (month <= 2U) {
        year -= 1;
    }
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year = (153U * shifted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                                year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static time_t utc_from_parts(const struct tm *parts)
{
    const int64_t days = days_from_civil(parts->tm_year + 1900,
                                         (unsigned)(parts->tm_mon + 1),
                                         (unsigned)parts->tm_mday);
    return (time_t)(days * 86400 + parts->tm_hour * 3600 +
                    parts->tm_min * 60 + parts->tm_sec);
}

static esp_err_t rtc_get_locked(time_t *utc)
{
    uint8_t raw[7];
    esp_err_t err = rtc_read(PCF85063A_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if ((raw[0] & PCF85063A_SECONDS_OS_MASK) != 0U) {
        /* Oscillator stopped since the last write: the held time is garbage. */
        return ESP_ERR_NOT_FOUND;
    }

    struct tm parts = {0};
    parts.tm_sec = from_bcd(raw[0] & 0x7FU);
    parts.tm_min = from_bcd(raw[1] & 0x7FU);
    parts.tm_hour = from_bcd(raw[2] & 0x3FU);
    parts.tm_mday = from_bcd(raw[3] & 0x3FU);
    /* raw[4] is the weekday, which the conversion below does not need. */
    parts.tm_mon = from_bcd(raw[5] & 0x1FU) - 1;
    parts.tm_year = from_bcd(raw[6]) + (PCF85063A_EPOCH_YEAR - 1900);
    parts.tm_isdst = 0;

    if (parts.tm_sec > 59 || parts.tm_min > 59 || parts.tm_hour > 23 ||
        parts.tm_mday < 1 || parts.tm_mday > 31 || parts.tm_mon < 0 ||
        parts.tm_mon > 11) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * The RTC holds UTC.  mktime would apply the process timezone, which the
     * night schedule sets, and timegm is not part of the guaranteed newlib
     * surface, so the conversion is done here: days since the epoch by the
     * usual civil-calendar formula, then seconds.
     */
    const time_t seconds = utc_from_parts(&parts);
    if (seconds <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *utc = seconds;
    return ESP_OK;
}

static esp_err_t rtc_set_locked(time_t utc)
{
    struct tm parts;
    if (gmtime_r(&utc, &parts) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int year = parts.tm_year + 1900;
    if (year < PCF85063A_EPOCH_YEAR || year > PCF85063A_EPOCH_YEAR + 99) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Halt the counter while the seven registers are rewritten. */
    uint8_t control = PCF85063A_CONTROL_1_STOP;
    esp_err_t err = rtc_write(PCF85063A_REG_CONTROL_1, &control, 1);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t raw[7] = {
        to_bcd((uint8_t)parts.tm_sec), /* writing clears the OS flag */
        to_bcd((uint8_t)parts.tm_min),
        to_bcd((uint8_t)parts.tm_hour),
        to_bcd((uint8_t)parts.tm_mday),
        (uint8_t)parts.tm_wday,
        to_bcd((uint8_t)(parts.tm_mon + 1)),
        to_bcd((uint8_t)(year - PCF85063A_EPOCH_YEAR)),
    };
    err = rtc_write(PCF85063A_REG_SECONDS, raw, sizeof(raw));

    control = 0;
    const esp_err_t resume = rtc_write(PCF85063A_REG_CONTROL_1, &control, 1);
    return err != ESP_OK ? err : resume;
}

#endif /* BOARD_HAS_RTC */

#if BOARD_HAS_ENVIRONMENT_SENSOR
/* -------------------------------------------------------------------- SHTC3 */

static uint8_t shtc3_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t shtc3_command(uint16_t command)
{
    if (s_sensors.environment == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[2] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit(s_sensors.environment, payload, sizeof(payload),
                               SENSORS_I2C_TIMEOUT_MS);
}

static esp_err_t shtc3_read(uint8_t *data, size_t length)
{
    if (s_sensors.environment == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(s_sensors.environment, data, length,
                              SENSORS_I2C_TIMEOUT_MS);
}

static esp_err_t shtc3_wake(void)
{
    const esp_err_t err = shtc3_command(SHTC3_CMD_WAKE);
    if (err == ESP_OK) {
        /* Datasheet: 240 us to leave sleep; one tick is the smallest wait. */
        vTaskDelay(1);
    }
    return err;
}

static esp_err_t shtc3_sample_locked(float *temperature_c, float *humidity)
{
    esp_err_t err = shtc3_wake();
    if (err != ESP_OK) {
        return err;
    }

    err = shtc3_command(SHTC3_CMD_MEASURE);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHTC3_MEASURE_DELAY_MS));
        uint8_t raw[6];
        err = shtc3_read(raw, sizeof(raw));
        if (err == ESP_OK) {
            if (shtc3_crc(&raw[0], 2) != raw[2] ||
                shtc3_crc(&raw[3], 2) != raw[5]) {
                err = ESP_ERR_INVALID_CRC;
            } else {
                const uint16_t t = (uint16_t)((raw[0] << 8) | raw[1]);
                const uint16_t rh = (uint16_t)((raw[3] << 8) | raw[4]);
                *temperature_c = -45.0f + (175.0f * (float)t / 65535.0f);
                *humidity = 100.0f * (float)rh / 65535.0f;
            }
        }
    }

    (void)shtc3_command(SHTC3_CMD_SLEEP);
    return err;
}

#endif /* BOARD_HAS_ENVIRONMENT_SENSOR */

/* --------------------------------------------------------------- public API */

esp_err_t board_sensors_init(void)
{
    if (s_sensors.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sensors.lock = xSemaphoreCreateMutexStatic(&s_sensors.lock_storage);
    if (s_sensors.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    (void)err;

#if BOARD_HAS_RTC
    err = attach(PCF85063A_ADDRESS, &s_sensors.rtc);
    if (err == ESP_OK) {
        time_t held = 0;
        const esp_err_t read_result = rtc_get_locked(&held);
        if (read_result == ESP_OK) {
            ESP_LOGI(TAG, "PCF85063A present, holding a valid time");
        } else if (read_result == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "PCF85063A present but its oscillator has stopped;"
                          " time comes from SNTP");
        } else {
            ESP_LOGW(TAG, "PCF85063A did not answer: %s",
                     esp_err_to_name(read_result));
            i2c_master_bus_rm_device(s_sensors.rtc);
            s_sensors.rtc = NULL;
        }
    } else {
        ESP_LOGW(TAG, "RTC not attachable: %s", esp_err_to_name(err));
        s_sensors.rtc = NULL;
    }
#endif

#if BOARD_HAS_ENVIRONMENT_SENSOR
    err = attach(SHTC3_ADDRESS, &s_sensors.environment);
    if (err == ESP_OK) {
        uint16_t id = 0;
        esp_err_t probe = shtc3_wake();
        if (probe == ESP_OK) {
            probe = shtc3_command(SHTC3_CMD_READ_ID);
        }
        if (probe == ESP_OK) {
            uint8_t raw[3];
            probe = shtc3_read(raw, sizeof(raw));
            if (probe == ESP_OK) {
                if (shtc3_crc(raw, 2) != raw[2]) {
                    probe = ESP_ERR_INVALID_CRC;
                } else {
                    id = (uint16_t)((raw[0] << 8) | raw[1]);
                }
            }
        }
        (void)shtc3_command(SHTC3_CMD_SLEEP);

        if (probe == ESP_OK && (id & SHTC3_ID_MASK) == SHTC3_ID_VALUE) {
            ESP_LOGI(TAG, "SHTC3 present (id 0x%04X)", id);
        } else {
            ESP_LOGW(TAG, "SHTC3 not identified (id 0x%04X, %s)", id,
                     esp_err_to_name(probe));
            i2c_master_bus_rm_device(s_sensors.environment);
            s_sensors.environment = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Environment sensor not attachable: %s",
                 esp_err_to_name(err));
        s_sensors.environment = NULL;
    }
#endif

    s_sensors.initialized = true;
    return ESP_OK;
}

esp_err_t board_sensors_get_status(board_sensors_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->supported = true;
    if (!s_sensors.initialized) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_sensors.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    status->rtc_present = s_sensors.rtc != NULL;
    status->environment_present = s_sensors.environment != NULL;
    status->environment_valid = s_sensors.environment_valid;
    status->temperature_c = s_sensors.temperature_c;
    status->humidity_percent = s_sensors.humidity_percent;
    if (status->rtc_present) {
        time_t held = 0;
        status->rtc_time_valid = rtc_get_locked(&held) == ESP_OK;
    }
    xSemaphoreGive(s_sensors.lock);
    return ESP_OK;
}

esp_err_t board_sensors_rtc_get(time_t *utc)
{
    if (utc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sensors.initialized || s_sensors.rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sensors.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = rtc_get_locked(utc);
    xSemaphoreGive(s_sensors.lock);
    return err;
}

esp_err_t board_sensors_rtc_set(time_t utc)
{
    if (!s_sensors.initialized || s_sensors.rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sensors.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = rtc_set_locked(utc);
    xSemaphoreGive(s_sensors.lock);
    return err;
}

esp_err_t board_sensors_read_environment(float *temperature_c,
                                         float *humidity_percent)
{
    if (!s_sensors.initialized || s_sensors.environment == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sensors.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    const int64_t now = monotonic_ms();
    const bool cache_fresh =
        s_sensors.environment_valid &&
        (now - s_sensors.environment_sampled_ms) < ENVIRONMENT_CACHE_MS;
    if (!cache_fresh) {
        float temperature = 0.0f;
        float humidity = 0.0f;
        err = shtc3_sample_locked(&temperature, &humidity);
        if (err == ESP_OK) {
            s_sensors.temperature_c = temperature;
            s_sensors.humidity_percent = humidity;
            s_sensors.environment_valid = true;
            s_sensors.environment_sampled_ms = now;
        } else if (s_sensors.environment_valid &&
                   (now - s_sensors.environment_sampled_ms) <
                       ENVIRONMENT_STALE_MS) {
            /* Keep serving the last good sample rather than nothing. */
            err = ESP_OK;
        } else {
            s_sensors.environment_valid = false;
        }
    }

    if (err == ESP_OK) {
        if (temperature_c != NULL) {
            *temperature_c = s_sensors.temperature_c;
        }
        if (humidity_percent != NULL) {
            *humidity_percent = s_sensors.humidity_percent;
        }
    }
    xSemaphoreGive(s_sensors.lock);
    return err;
}

#else /* board without optional sensors */

esp_err_t board_sensors_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_sensors_get_status(board_sensors_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = (board_sensors_status_t){0};
    return ESP_OK;
}

esp_err_t board_sensors_rtc_get(time_t *utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_sensors_rtc_set(time_t utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_sensors_read_environment(float *temperature_c,
                                         float *humidity_percent)
{
    (void)temperature_c;
    (void)humidity_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
