#include "connectivity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "connectivity";

#define CONNECTIVITY_SCAN_MAX_RAW_RECORDS 64U

/* DHCP Option 114 retains this pointer for the lifetime of the server. */
static char s_captive_portal_uri[] = "http://" CONNECTIVITY_SOFTAP_IPV4 "/";

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control_lock;
static esp_netif_t *s_station_netif;
static esp_netif_t *s_softap_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static bool s_initialized;
static bool s_wifi_started;
static bool s_station_requested;
static bool s_softap_requested;
static char s_station_ssid[CONNECTIVITY_SSID_MAX_BYTES + 1U];
static connectivity_status_t s_status;

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (destination_size == 0U) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    const size_t length = strnlen(source, destination_size - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static connectivity_mode_t requested_mode_locked(void)
{
    if (s_station_requested && s_softap_requested) {
        return CONNECTIVITY_MODE_AP_STATION;
    }
    if (s_station_requested) {
        return CONNECTIVITY_MODE_STATION;
    }
    if (s_softap_requested) {
        return CONNECTIVITY_MODE_SOFTAP;
    }
    return CONNECTIVITY_MODE_OFF;
}

static void refresh_active_interface_locked(void)
{
    s_status.mode = requested_mode_locked();
    s_status.station_enabled = s_station_requested;
    s_status.softap_enabled = s_softap_requested;

    if (s_status.connected) {
        copy_text(s_status.ssid, sizeof(s_status.ssid), s_station_ssid);
        return;
    }

    s_status.rssi_available = false;
    s_status.rssi_dbm = 0;
    if (s_softap_requested) {
        copy_text(s_status.ssid, sizeof(s_status.ssid), s_status.softap_ssid);
        copy_text(s_status.ip_address, sizeof(s_status.ip_address),
                  CONNECTIVITY_SOFTAP_IPV4);
    } else if (s_station_requested) {
        copy_text(s_status.ssid, sizeof(s_status.ssid), s_station_ssid);
        s_status.ip_address[0] = '\0';
    } else {
        s_status.ssid[0] = '\0';
        s_status.ip_address[0] = '\0';
    }
}

static bool station_is_requested(void)
{
    bool requested;
    taskENTER_CRITICAL(&s_status_lock);
    requested = s_station_requested;
    taskEXIT_CRITICAL(&s_status_lock);
    return requested;
}

static void clear_station_link(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.connected = false;
    s_status.ip_address[0] = '\0';
    refresh_active_interface_locked();
    taskEXIT_CRITICAL(&s_status_lock);
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_START) {
        if (station_is_requested()) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Station connect could not be started: %s",
                         esp_err_to_name(result));
            }
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        clear_station_link();
        if (station_is_requested()) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Station reconnect could not be started: %s",
                         esp_err_to_name(result));
            }
        }
    } else if (event_id == WIFI_EVENT_STA_STOP) {
        clear_station_link();
    }
}

static void ip_event_handler(void *argument, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base != IP_EVENT) {
        return;
    }
    if (event_id == IP_EVENT_STA_LOST_IP) {
        clear_station_link();
        return;
    }
    if (event_id != IP_EVENT_STA_GOT_IP || event_data == NULL ||
        !station_is_requested()) {
        return;
    }

    const ip_event_got_ip_t *event = event_data;
    char ip_address[CONNECTIVITY_IPV4_TEXT_MAX_BYTES + 1U];
    snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&event->ip_info.ip));

    wifi_ap_record_t access_point = {0};
    const bool have_rssi = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

    taskENTER_CRITICAL(&s_status_lock);
    if (s_station_requested) {
        s_status.connected = true;
        s_status.rssi_available = have_rssi;
        s_status.rssi_dbm = have_rssi ? access_point.rssi : 0;
        copy_text(s_status.ssid, sizeof(s_status.ssid), s_station_ssid);
        copy_text(s_status.ip_address, sizeof(s_status.ip_address), ip_address);
        s_status.mode = requested_mode_locked();
        s_status.station_enabled = true;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t validate_ssid(const char *ssid, size_t *length)
{
    if (ssid == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t value_length = strnlen(ssid, CONNECTIVITY_SSID_MAX_BYTES + 1U);
    if (value_length == 0U || value_length > CONNECTIVITY_SSID_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    *length = value_length;
    return ESP_OK;
}

static esp_err_t validate_password(const char *password, bool allow_open,
                                   size_t *length)
{
    if (password == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t value_length =
        strnlen(password, CONNECTIVITY_PASSWORD_MAX_BYTES + 1U);
    if (value_length > CONNECTIVITY_PASSWORD_MAX_BYTES ||
        (!allow_open && value_length == 0U) ||
        (value_length > 0U && value_length < 8U)) {
        return ESP_ERR_INVALID_ARG;
    }

    *length = value_length;
    return ESP_OK;
}

static wifi_mode_t requested_wifi_mode(void)
{
    bool station;
    bool softap;
    taskENTER_CRITICAL(&s_status_lock);
    station = s_station_requested;
    softap = s_softap_requested;
    taskEXIT_CRITICAL(&s_status_lock);

    if (station && softap) {
        return WIFI_MODE_APSTA;
    }
    if (station) {
        return WIFI_MODE_STA;
    }
    if (softap) {
        return WIFI_MODE_AP;
    }
    return WIFI_MODE_NULL;
}

static esp_err_t set_wifi_mode_and_start(wifi_mode_t mode)
{
    if (mode == WIFI_MODE_NULL) {
        if (!s_wifi_started) {
            return ESP_OK;
        }
        const esp_err_t stop_result = esp_wifi_stop();
        if (stop_result == ESP_OK) {
            s_wifi_started = false;
        }
        return stop_result;
    }

    esp_err_t result = esp_wifi_set_mode(mode);
    if (result != ESP_OK) {
        return result;
    }

    if (!s_wifi_started) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            s_wifi_started = true;
        }
    }
    return result;
}

static esp_err_t configure_softap_ipv4(void)
{
    esp_err_t result = esp_netif_dhcps_stop(s_softap_netif);
    if (result != ESP_OK &&
        result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return result;
    }

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
    result = esp_netif_set_ip_info(s_softap_netif, &ip_info);
    if (result != ESP_OK) {
        return result;
    }

    uint8_t offer_dns = 1U;
    result = esp_netif_dhcps_option(
        s_softap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns, sizeof(offer_dns));
    if (result != ESP_OK) {
        return result;
    }

    esp_netif_dns_info_t dns_info = {0};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = ip_info.ip.addr;
    result = esp_netif_set_dns_info(s_softap_netif, ESP_NETIF_DNS_MAIN,
                                    &dns_info);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_netif_dhcps_option(
        s_softap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        s_captive_portal_uri, strlen(s_captive_portal_uri));
    if (result != ESP_OK) {
        return result;
    }

    /*
     * Before AP_START the netif is down. Starting DHCPS here moves its state
     * back to INIT, so the default AP_START handler starts it with the address
     * above as soon as the interface comes up.
     */
    return esp_netif_dhcps_start(s_softap_netif);
}

esp_err_t connectivity_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_control_lock = xSemaphoreCreateMutex();
    if (s_control_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    s_station_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_station_netif == NULL) {
        s_station_netif = esp_netif_create_default_wifi_sta();
    }
    s_softap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_softap_netif == NULL) {
        s_softap_netif = esp_netif_create_default_wifi_ap();
    }
    if (s_station_netif == NULL || s_softap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    result = configure_softap_ipv4();
    if (result != ESP_OK) {
        return result;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config.nvs_enable = false;
    result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
        &s_wifi_event_instance);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL,
        &s_ip_event_instance);
    if (result != ESP_OK) {
        return result;
    }

    taskENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    copy_text(s_status.softap_ip_address,
              sizeof(s_status.softap_ip_address), CONNECTIVITY_SOFTAP_IPV4);
    s_initialized = true;
    taskEXIT_CRITICAL(&s_status_lock);

    ESP_LOGI(TAG, "Wi-Fi initialized with volatile driver storage");
    return ESP_OK;
}

esp_err_t connectivity_start_station(const char *ssid, const char *password)
{
    size_t ssid_length;
    size_t password_length;
    esp_err_t result = validate_ssid(ssid, &ssid_length);
    if (result != ESP_OK) {
        return result;
    }
    result = validate_password(password, true, &password_length);
    if (result != ESP_OK) {
        return result;
    }
    result = connectivity_init();
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(s_control_lock, portMAX_DELAY);

    bool station_was_active;
    taskENTER_CRITICAL(&s_status_lock);
    station_was_active = s_wifi_started && s_station_requested;
    s_station_requested = false;
    taskEXIT_CRITICAL(&s_status_lock);
    if (s_wifi_started) {
        const esp_err_t disconnect_result = esp_wifi_disconnect();
        if (disconnect_result != ESP_OK &&
            disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Could not clear the previous station link: %s",
                     esp_err_to_name(disconnect_result));
        }
    }

    wifi_mode_t mode;
    taskENTER_CRITICAL(&s_status_lock);
    mode = s_softap_requested ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    taskEXIT_CRITICAL(&s_status_lock);
    result = esp_wifi_set_mode(mode);
    if (result != ESP_OK) {
        goto done;
    }

    wifi_config_t station_config = {0};
    memcpy(station_config.sta.ssid, ssid, ssid_length);
    memcpy(station_config.sta.password, password, password_length);
    station_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    station_config.sta.threshold.authmode =
        password_length == 0U ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    station_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    result = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (result != ESP_OK) {
        goto done;
    }

    taskENTER_CRITICAL(&s_status_lock);
    memcpy(s_station_ssid, ssid, ssid_length);
    s_station_ssid[ssid_length] = '\0';
    s_station_requested = true;
    s_status.connected = false;
    refresh_active_interface_locked();
    taskEXIT_CRITICAL(&s_status_lock);

    result = set_wifi_mode_and_start(mode);
    /* A newly enabled STA interface connects from WIFI_EVENT_STA_START. When
     * replacing credentials on an already-running STA there is no new start
     * event, so initiate that one case here. */
    if (result == ESP_OK && s_wifi_started && station_was_active) {
        const esp_err_t connect_result = esp_wifi_connect();
        if (connect_result != ESP_OK && connect_result != ESP_ERR_WIFI_CONN) {
            result = connect_result;
        }
    }

done:
    if (result != ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        s_station_requested = false;
        s_status.connected = false;
        refresh_active_interface_locked();
        taskEXIT_CRITICAL(&s_status_lock);
    }
    xSemaphoreGive(s_control_lock);
    return result;
}

esp_err_t connectivity_stop_station(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_status_lock);
    s_station_requested = false;
    s_status.connected = false;
    s_station_ssid[0] = '\0';
    refresh_active_interface_locked();
    taskEXIT_CRITICAL(&s_status_lock);

    if (s_wifi_started) {
        const esp_err_t disconnect_result = esp_wifi_disconnect();
        if (disconnect_result != ESP_OK &&
            disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Station disconnect returned: %s",
                     esp_err_to_name(disconnect_result));
        }
    }
    const esp_err_t result = set_wifi_mode_and_start(requested_wifi_mode());
    xSemaphoreGive(s_control_lock);
    return result;
}

esp_err_t connectivity_start_softap(const char *ssid, const char *password)
{
    size_t ssid_length;
    size_t password_length;
    esp_err_t result = validate_ssid(ssid, &ssid_length);
    if (result != ESP_OK) {
        return result;
    }
    result = validate_password(password, false, &password_length);
    if (result != ESP_OK) {
        return result;
    }
    result = connectivity_init();
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(s_control_lock, portMAX_DELAY);

    wifi_mode_t mode;
    taskENTER_CRITICAL(&s_status_lock);
    mode = s_station_requested ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    taskEXIT_CRITICAL(&s_status_lock);
    result = esp_wifi_set_mode(mode);
    if (result != ESP_OK) {
        goto done;
    }

    wifi_config_t softap_config = {0};
    memcpy(softap_config.ap.ssid, ssid, ssid_length);
    memcpy(softap_config.ap.password, password, password_length);
    softap_config.ap.ssid_len = ssid_length;
    softap_config.ap.channel = 1;
    softap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    softap_config.ap.max_connection = 3;
    softap_config.ap.pmf_cfg.required = false;
    result = esp_wifi_set_config(WIFI_IF_AP, &softap_config);
    if (result != ESP_OK) {
        goto done;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_softap_requested = true;
    memcpy(s_status.softap_ssid, ssid, ssid_length);
    s_status.softap_ssid[ssid_length] = '\0';
    copy_text(s_status.softap_ip_address,
              sizeof(s_status.softap_ip_address), CONNECTIVITY_SOFTAP_IPV4);
    refresh_active_interface_locked();
    taskEXIT_CRITICAL(&s_status_lock);

    result = set_wifi_mode_and_start(mode);

done:
    if (result != ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        s_softap_requested = false;
        s_status.softap_ssid[0] = '\0';
        refresh_active_interface_locked();
        taskEXIT_CRITICAL(&s_status_lock);
    }
    xSemaphoreGive(s_control_lock);
    return result;
}

esp_err_t connectivity_stop_softap(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_status_lock);
    s_softap_requested = false;
    s_status.softap_ssid[0] = '\0';
    refresh_active_interface_locked();
    taskEXIT_CRITICAL(&s_status_lock);

    const esp_err_t result = set_wifi_mode_and_start(requested_wifi_mode());
    xSemaphoreGive(s_control_lock);
    return result;
}

static bool network_is_already_listed(const connectivity_network_t *networks,
                                      size_t count, const char *ssid,
                                      size_t *existing_index)
{
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(networks[index].ssid, ssid) == 0) {
            if (existing_index != NULL) {
                *existing_index = index;
            }
            return true;
        }
    }
    return false;
}

static void sort_networks_by_rssi(connectivity_network_t *networks,
                                  size_t count)
{
    for (size_t index = 1U; index < count; ++index) {
        const connectivity_network_t candidate = networks[index];
        size_t insertion_index = index;
        while (insertion_index > 0U &&
               networks[insertion_index - 1U].rssi_dbm < candidate.rssi_dbm) {
            networks[insertion_index] = networks[insertion_index - 1U];
            --insertion_index;
        }
        networks[insertion_index] = candidate;
    }
}

esp_err_t connectivity_scan_networks(connectivity_network_t *networks,
                                     size_t capacity, size_t *count)
{
    if (count == NULL || (capacity > 0U && networks == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    if (capacity == 0U) {
        return ESP_OK;
    }

    esp_err_t result = connectivity_init();
    if (result != ESP_OK) {
        return result;
    }

    const size_t result_capacity =
        capacity < CONNECTIVITY_SCAN_MAX_RESULTS
            ? capacity
            : CONNECTIVITY_SCAN_MAX_RESULTS;
    wifi_ap_record_t *records = NULL;
    bool scan_records_need_clear = false;

    xSemaphoreTake(s_control_lock, portMAX_DELAY);

    const wifi_mode_t requested_mode = requested_wifi_mode();
    wifi_mode_t scan_mode = requested_mode;
    if (requested_mode == WIFI_MODE_AP) {
        scan_mode = WIFI_MODE_APSTA;
    } else if (requested_mode == WIFI_MODE_NULL) {
        scan_mode = WIFI_MODE_STA;
    }

    const bool mode_changed = scan_mode != requested_mode;
    if (mode_changed) {
        result = set_wifi_mode_and_start(scan_mode);
        if (result != ESP_OK) {
            goto restore_mode;
        }
    } else if (!s_wifi_started) {
        result = set_wifi_mode_and_start(scan_mode);
        if (result != ESP_OK) {
            goto restore_mode;
        }
    }

    const wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    result = esp_wifi_scan_start(&scan_config, true);
    if (result != ESP_OK) {
        goto restore_mode;
    }
    scan_records_need_clear = true;

    uint16_t record_count = 0U;
    result = esp_wifi_scan_get_ap_num(&record_count);
    if (result != ESP_OK) {
        goto clear_records;
    }
    if (record_count == 0U) {
        result = ESP_OK;
        goto clear_records;
    }
    if (record_count > CONNECTIVITY_SCAN_MAX_RAW_RECORDS) {
        record_count = CONNECTIVITY_SCAN_MAX_RAW_RECORDS;
    }

    records = calloc(record_count, sizeof(*records));
    if (records == NULL) {
        result = ESP_ERR_NO_MEM;
        goto clear_records;
    }

    result = esp_wifi_scan_get_ap_records(&record_count, records);
    if (result != ESP_OK) {
        goto clear_records;
    }
    scan_records_need_clear = false;

    for (uint16_t record_index = 0U; record_index < record_count;
         ++record_index) {
        const wifi_ap_record_t *record = &records[record_index];
        const size_t ssid_length = strnlen(
            (const char *)record->ssid, CONNECTIVITY_SSID_MAX_BYTES);
        if (ssid_length == 0U) {
            continue;
        }

        char ssid[CONNECTIVITY_SSID_MAX_BYTES + 1U];
        memcpy(ssid, record->ssid, ssid_length);
        ssid[ssid_length] = '\0';

        size_t existing_index = 0U;
        if (network_is_already_listed(networks, *count, ssid,
                                      &existing_index)) {
            if (record->rssi > networks[existing_index].rssi_dbm) {
                networks[existing_index].rssi_dbm = record->rssi;
                networks[existing_index].secured =
                    record->authmode != WIFI_AUTH_OPEN;
            }
            continue;
        }
        if (*count >= result_capacity) {
            continue;
        }

        connectivity_network_t *network = &networks[*count];
        memcpy(network->ssid, ssid, ssid_length + 1U);
        network->rssi_dbm = record->rssi;
        network->secured = record->authmode != WIFI_AUTH_OPEN;
        ++(*count);
    }
    sort_networks_by_rssi(networks, *count);
    result = ESP_OK;

clear_records:
    if (scan_records_need_clear) {
        const esp_err_t clear_result = esp_wifi_clear_ap_list();
        if (result == ESP_OK && clear_result != ESP_OK) {
            result = clear_result;
        }
    }
    free(records);

restore_mode:
    if (mode_changed) {
        const esp_err_t restore_result =
            set_wifi_mode_and_start(requested_mode);
        if (result == ESP_OK && restore_result != ESP_OK) {
            result = restore_result;
        }
    }
    xSemaphoreGive(s_control_lock);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi scan returned %u unique networks",
                 (unsigned)*count);
    }
    return result;
}

esp_err_t connectivity_get_status(connectivity_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
