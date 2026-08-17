#include "captive_dns.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define CAPTIVE_DNS_PORT 53U
#define CAPTIVE_DNS_MAX_PACKET_BYTES 512U
#define CAPTIVE_DNS_RECEIVE_BYTES (CAPTIVE_DNS_MAX_PACKET_BYTES + 1U)
#define CAPTIVE_DNS_HEADER_BYTES 12U
#define CAPTIVE_DNS_MAX_NAME_BYTES 255U
#define CAPTIVE_DNS_TASK_STACK_BYTES 3072U
#define CAPTIVE_DNS_RECEIVE_TIMEOUT_MS 250U
#define CAPTIVE_DNS_STOP_TIMEOUT_MS 1500U
#define CAPTIVE_DNS_ANSWER_TTL_SECONDS 30U

#define DNS_FLAG_QR 0x8000U
#define DNS_FLAG_OPCODE 0x7800U
#define DNS_FLAG_AA 0x0400U
#define DNS_FLAG_TC 0x0200U
#define DNS_FLAG_RD 0x0100U

#define DNS_TYPE_A 1U
#define DNS_TYPE_ANY 255U
#define DNS_CLASS_IN 1U

static const char *TAG = "captive_dns";

typedef struct {
    uint16_t request_flags;
    uint16_t question_type;
    uint16_t question_class;
    size_t question_name_length;
} dns_question_t;

typedef enum {
    DNS_PARSE_INVALID = 0,
    DNS_PARSE_IGNORE,
    DNS_PARSE_VALID,
} dns_parse_result_t;

typedef struct {
    uint8_t receive_buffer[CAPTIVE_DNS_RECEIVE_BYTES];
    uint8_t response_buffer[CAPTIVE_DNS_MAX_PACKET_BYTES];
    uint8_t question_name[CAPTIVE_DNS_MAX_NAME_BYTES];
    struct in_addr redirect_address;
    char redirect_address_text[INET_ADDRSTRLEN];
} captive_dns_context_t;

static captive_dns_context_t s_dns;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_control_busy;
static bool s_running;
static TaskHandle_t s_task;
static int s_socket = -1;
static bool s_stop_requested;
static StaticSemaphore_t s_stopped_signal_storage;
static SemaphoreHandle_t s_stopped_signal;

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void control_enter(void)
{
    for (;;) {
        bool acquired = false;
        taskENTER_CRITICAL(&s_state_lock);
        if (!s_control_busy) {
            s_control_busy = true;
            acquired = true;
        }
        taskEXIT_CRITICAL(&s_state_lock);

        if (acquired) {
            return;
        }
        vTaskDelay(1U);
    }
}

static void control_exit(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_control_busy = false;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool stop_was_requested(void)
{
    bool stop_requested;
    taskENTER_CRITICAL(&s_state_lock);
    stop_requested = s_stop_requested;
    taskEXIT_CRITICAL(&s_state_lock);
    return stop_requested;
}

static bool offset_was_visited(const uint8_t *visited, size_t offset)
{
    return (visited[offset / 8U] & (uint8_t)(1U << (offset % 8U))) != 0U;
}

static void mark_offset_visited(uint8_t *visited, size_t offset)
{
    visited[offset / 8U] |= (uint8_t)(1U << (offset % 8U));
}

/*
 * Decode a DNS wire-format name and report how many bytes it occupied at its
 * original location. Compression is deliberately disabled for the sole query
 * name: with exactly one question there is no prior name to reference. It is
 * enabled while validating later resource records, with backward-only
 * pointers and per-offset loop detection.
 */
static bool decode_name(const uint8_t *packet, size_t packet_length,
                        size_t name_offset, bool allow_compression,
                        uint8_t *decoded, size_t decoded_capacity,
                        size_t *wire_length, size_t *decoded_length)
{
    uint8_t visited[(CAPTIVE_DNS_MAX_PACKET_BYTES + 7U) / 8U] = {0};
    size_t cursor = name_offset;
    size_t source_end = 0U;
    size_t output_length = 0U;
    bool followed_pointer = false;

    if (packet == NULL || wire_length == NULL || decoded_length == NULL ||
        name_offset >= packet_length ||
        packet_length > CAPTIVE_DNS_MAX_PACKET_BYTES) {
        return false;
    }

    for (size_t step = 0U; step < CAPTIVE_DNS_MAX_PACKET_BYTES; ++step) {
        if (cursor >= packet_length || offset_was_visited(visited, cursor)) {
            return false;
        }
        mark_offset_visited(visited, cursor);

        const uint8_t label_length = packet[cursor];
        if ((label_length & 0xC0U) == 0xC0U) {
            if (!allow_compression || cursor + 1U >= packet_length) {
                return false;
            }

            const size_t pointer =
                (size_t)(((uint16_t)(label_length & 0x3FU) << 8U) |
                         packet[cursor + 1U]);
            if (pointer < CAPTIVE_DNS_HEADER_BYTES || pointer >= cursor ||
                pointer >= packet_length) {
                return false;
            }
            if (!followed_pointer) {
                source_end = cursor + 2U;
                followed_pointer = true;
            }
            cursor = pointer;
            continue;
        }
        if ((label_length & 0xC0U) != 0U) {
            return false;
        }

        if (label_length == 0U) {
            if (!followed_pointer) {
                source_end = cursor + 1U;
            }
            if (output_length + 1U > CAPTIVE_DNS_MAX_NAME_BYTES ||
                (decoded != NULL && output_length + 1U > decoded_capacity)) {
                return false;
            }
            if (decoded != NULL) {
                decoded[output_length] = 0U;
            }
            ++output_length;
            *wire_length = source_end - name_offset;
            *decoded_length = output_length;
            return true;
        }

        if (label_length > 63U ||
            cursor + 1U + label_length > packet_length ||
            output_length + 1U + label_length + 1U >
                CAPTIVE_DNS_MAX_NAME_BYTES ||
            (decoded != NULL &&
             output_length + 1U + label_length > decoded_capacity)) {
            return false;
        }

        if (decoded != NULL) {
            decoded[output_length] = label_length;
            memcpy(decoded + output_length + 1U, packet + cursor + 1U,
                   label_length);
        }
        output_length += 1U + label_length;
        cursor += 1U + label_length;
    }

    return false;
}

static bool validate_resource_records(const uint8_t *packet,
                                      size_t packet_length, size_t *cursor,
                                      uint32_t record_count)
{
    for (uint32_t index = 0U; index < record_count; ++index) {
        size_t name_wire_length = 0U;
        size_t name_decoded_length = 0U;
        if (!decode_name(packet, packet_length, *cursor, true, NULL, 0U,
                         &name_wire_length, &name_decoded_length)) {
            return false;
        }
        (void)name_decoded_length;

        if (name_wire_length > packet_length - *cursor) {
            return false;
        }
        *cursor += name_wire_length;
        if (*cursor > packet_length || packet_length - *cursor < 10U) {
            return false;
        }

        const uint16_t data_length = read_u16(packet + *cursor + 8U);
        *cursor += 10U;
        if ((size_t)data_length > packet_length - *cursor) {
            return false;
        }
        *cursor += data_length;
    }
    return true;
}

static dns_parse_result_t parse_query(const uint8_t *packet,
                                      size_t packet_length,
                                      dns_question_t *question)
{
    if (packet == NULL || question == NULL ||
        packet_length < CAPTIVE_DNS_HEADER_BYTES ||
        packet_length > CAPTIVE_DNS_MAX_PACKET_BYTES) {
        return DNS_PARSE_INVALID;
    }

    const uint16_t flags = read_u16(packet + 2U);
    if ((flags & DNS_FLAG_QR) != 0U) {
        return DNS_PARSE_IGNORE;
    }
    if ((flags & DNS_FLAG_OPCODE) != 0U || (flags & DNS_FLAG_TC) != 0U ||
        read_u16(packet + 4U) != 1U) {
        return DNS_PARSE_INVALID;
    }

    size_t name_wire_length = 0U;
    if (!decode_name(packet, packet_length, CAPTIVE_DNS_HEADER_BYTES, false,
                     s_dns.question_name, sizeof(s_dns.question_name),
                     &name_wire_length, &question->question_name_length)) {
        return DNS_PARSE_INVALID;
    }

    size_t cursor = CAPTIVE_DNS_HEADER_BYTES + name_wire_length;
    if (cursor > packet_length || packet_length - cursor < 4U) {
        return DNS_PARSE_INVALID;
    }

    question->request_flags = flags;
    question->question_type = read_u16(packet + cursor);
    question->question_class = read_u16(packet + cursor + 2U);
    cursor += 4U;

    const uint32_t resource_record_count =
        (uint32_t)read_u16(packet + 6U) +
        (uint32_t)read_u16(packet + 8U) +
        (uint32_t)read_u16(packet + 10U);
    if (!validate_resource_records(packet, packet_length, &cursor,
                                   resource_record_count) ||
        cursor != packet_length) {
        return DNS_PARSE_INVALID;
    }

    return DNS_PARSE_VALID;
}

static size_t build_response(const uint8_t *request,
                             const dns_question_t *question,
                             uint8_t *response, size_t response_capacity)
{
    const bool include_answer =
        question->question_class == DNS_CLASS_IN &&
        (question->question_type == DNS_TYPE_A ||
         question->question_type == DNS_TYPE_ANY);
    const size_t question_bytes = question->question_name_length + 4U;
    const size_t answer_bytes = include_answer ? 16U : 0U;
    const size_t response_length =
        CAPTIVE_DNS_HEADER_BYTES + question_bytes + answer_bytes;

    if (response_length > response_capacity) {
        return 0U;
    }

    memset(response, 0, response_length);
    response[0] = request[0];
    response[1] = request[1];
    write_u16(response + 2U,
              (uint16_t)(DNS_FLAG_QR | DNS_FLAG_AA |
                         (question->request_flags & DNS_FLAG_RD)));
    write_u16(response + 4U, 1U);
    write_u16(response + 6U, include_answer ? 1U : 0U);

    size_t cursor = CAPTIVE_DNS_HEADER_BYTES;
    memcpy(response + cursor, s_dns.question_name,
           question->question_name_length);
    cursor += question->question_name_length;
    write_u16(response + cursor, question->question_type);
    write_u16(response + cursor + 2U, question->question_class);
    cursor += 4U;

    if (include_answer) {
        write_u16(response + cursor, 0xC00CU);
        write_u16(response + cursor + 2U, DNS_TYPE_A);
        write_u16(response + cursor + 4U, DNS_CLASS_IN);
        write_u32(response + cursor + 6U, CAPTIVE_DNS_ANSWER_TTL_SECONDS);
        write_u16(response + cursor + 10U, sizeof(s_dns.redirect_address));
        memcpy(response + cursor + 12U, &s_dns.redirect_address,
               sizeof(s_dns.redirect_address));
    }

    return response_length;
}

static void captive_dns_task(void *argument)
{
    (void)argument;

    int socket_fd;
    taskENTER_CRITICAL(&s_state_lock);
    socket_fd = s_socket;
    taskEXIT_CRITICAL(&s_state_lock);

    while (!stop_was_requested()) {
        struct sockaddr_in source_address = {0};
        socklen_t source_address_length = sizeof(source_address);
        const ssize_t received =
            recvfrom(socket_fd, s_dns.receive_buffer,
                     sizeof(s_dns.receive_buffer), 0,
                     (struct sockaddr *)&source_address,
                     &source_address_length);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (!stop_was_requested()) {
                ESP_LOGW(TAG, "DNS receive failed: errno %d", errno);
            }
            break;
        }
        if (received == 0 ||
            (size_t)received > CAPTIVE_DNS_MAX_PACKET_BYTES) {
            continue;
        }

        dns_question_t question = {0};
        const dns_parse_result_t parse_result =
            parse_query(s_dns.receive_buffer, (size_t)received, &question);
        if (parse_result != DNS_PARSE_VALID) {
            continue;
        }

        const size_t response_length =
            build_response(s_dns.receive_buffer, &question,
                           s_dns.response_buffer,
                           sizeof(s_dns.response_buffer));
        if (response_length == 0U) {
            continue;
        }

        const ssize_t sent =
            sendto(socket_fd, s_dns.response_buffer, response_length, 0,
                   (const struct sockaddr *)&source_address,
                   source_address_length);
        if (sent < 0 || (size_t)sent != response_length) {
            ESP_LOGD(TAG, "DNS reply failed: errno %d", errno);
        }
    }

    close(socket_fd);

    taskENTER_CRITICAL(&s_state_lock);
    if (s_socket == socket_fd) {
        s_socket = -1;
    }
    s_running = false;
    taskEXIT_CRITICAL(&s_state_lock);

    xSemaphoreGive(s_stopped_signal);

    taskENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "Captive DNS stopped");
    vTaskDelete(NULL);
}

static esp_err_t configure_socket(int *socket_fd)
{
    int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (descriptor < 0) {
        ESP_LOGE(TAG, "Could not create DNS socket: errno %d", errno);
        return ESP_FAIL;
    }

    const struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = CAPTIVE_DNS_RECEIVE_TIMEOUT_MS * 1000U,
    };
    if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Could not set DNS receive timeout: errno %d", errno);
        close(descriptor);
        return ESP_FAIL;
    }

    const int reuse_address = 1;
    (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                     sizeof(reuse_address));

    const struct sockaddr_in bind_address = {
        .sin_family = AF_INET,
        .sin_port = htons(CAPTIVE_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(descriptor, (const struct sockaddr *)&bind_address,
             sizeof(bind_address)) < 0) {
        ESP_LOGE(TAG, "Could not bind UDP port %u: errno %d",
                 CAPTIVE_DNS_PORT, errno);
        close(descriptor);
        return ESP_FAIL;
    }

    *socket_fd = descriptor;
    return ESP_OK;
}

esp_err_t captive_dns_start(const char *ipv4_address)
{
    if (ipv4_address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct in_addr parsed_address = {0};
    if (inet_pton(AF_INET, ipv4_address, &parsed_address) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    control_enter();

    taskENTER_CRITICAL(&s_state_lock);
    const bool already_started = s_task != NULL || s_running;
    taskEXIT_CRITICAL(&s_state_lock);
    if (already_started) {
        control_exit();
        return ESP_ERR_INVALID_STATE;
    }

    int socket_fd = -1;
    esp_err_t result = configure_socket(&socket_fd);
    if (result != ESP_OK) {
        control_exit();
        return result;
    }

    memset(&s_dns, 0, sizeof(s_dns));
    s_dns.redirect_address = parsed_address;
    if (inet_ntop(AF_INET, &parsed_address, s_dns.redirect_address_text,
                  sizeof(s_dns.redirect_address_text)) == NULL) {
        close(socket_fd);
        control_exit();
        return ESP_ERR_INVALID_ARG;
    }

    s_stopped_signal =
        xSemaphoreCreateBinaryStatic(&s_stopped_signal_storage);
    TaskHandle_t task = NULL;
    vTaskSuspendAll();
    const BaseType_t task_result =
        xTaskCreate(captive_dns_task, "captive_dns",
                    CAPTIVE_DNS_TASK_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 1U, &task);
    if (task_result == pdPASS) {
        taskENTER_CRITICAL(&s_state_lock);
        s_socket = socket_fd;
        s_task = task;
        s_running = true;
        s_stop_requested = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    (void)xTaskResumeAll();

    if (task_result != pdPASS) {
        close(socket_fd);
        control_exit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Captive DNS redirecting to %s",
             s_dns.redirect_address_text);
    control_exit();
    return ESP_OK;
}

esp_err_t captive_dns_stop(void)
{
    control_enter();

    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_state_lock);
    const TaskHandle_t task = s_task;
    const bool can_stop = task != NULL && task != current_task;
    if (can_stop) {
        s_stop_requested = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!can_stop) {
        control_exit();
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_stopped_signal,
                       pdMS_TO_TICKS(CAPTIVE_DNS_STOP_TIMEOUT_MS)) != pdTRUE) {
        control_exit();
        return ESP_ERR_TIMEOUT;
    }

    /* The signal can preempt the low-priority DNS task immediately before it
     * clears its handle. Give it a bounded opportunity to finish that step. */
    for (size_t attempt = 0U; attempt < 20U; ++attempt) {
        taskENTER_CRITICAL(&s_state_lock);
        const bool task_exited = s_task == NULL;
        taskEXIT_CRITICAL(&s_state_lock);
        if (task_exited) {
            control_exit();
            return ESP_OK;
        }
        vTaskDelay(1U);
    }

    control_exit();
    return ESP_ERR_TIMEOUT;
}

bool captive_dns_is_running(void)
{
    bool running;
    taskENTER_CRITICAL(&s_state_lock);
    running = s_running;
    taskEXIT_CRITICAL(&s_state_lock);
    return running;
}
