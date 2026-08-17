#pragma once
#include "freertos/FreeRTOS.h"
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
static inline uint32_t ulTaskNotifyTakeIndexed(unsigned idx, BaseType_t clear, TickType_t wait) { (void)idx; (void)clear; (void)wait; return 1; }
static inline void vTaskNotifyGiveIndexedFromISR(TaskHandle_t t, unsigned idx, BaseType_t *woken) { (void)t; (void)idx; *woken = pdFALSE; }
