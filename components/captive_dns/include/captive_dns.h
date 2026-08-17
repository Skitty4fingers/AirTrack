#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the captive DNS redirector on UDP port 53.
 *
 * Valid Internet-class A and ANY questions are answered with ipv4_address.
 * Other question types receive a successful response with no answers.
 * ipv4_address must be a dotted-decimal IPv4 address and is copied by the
 * component.
 *
 * This function must be called from task context.
 */
esp_err_t captive_dns_start(const char *ipv4_address);

/** Stop the redirector and wait for its socket-owning task to exit. */
esp_err_t captive_dns_stop(void);

/** Return true while the redirector task is accepting DNS requests. */
bool captive_dns_is_running(void);

#ifdef __cplusplus
}
#endif
