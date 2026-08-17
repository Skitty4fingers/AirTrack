#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
typedef struct { int task_priority; int task_stack; int task_affinity; int task_max_sleep_ms; unsigned task_stack_caps; int timer_period_ms; } lvgl_port_cfg_t;
typedef enum { LVGL_PORT_EVENT_USER = 1 } lvgl_port_event_type_t;
static inline esp_err_t lvgl_port_init(const lvgl_port_cfg_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t lvgl_port_deinit(void) { return ESP_OK; }
static inline bool lvgl_port_lock(uint32_t ms) { (void)ms; return true; }
static inline void lvgl_port_unlock(void) {}
static inline esp_err_t lvgl_port_task_wake(lvgl_port_event_type_t e, void *p) { (void)e; (void)p; return ESP_OK; }
