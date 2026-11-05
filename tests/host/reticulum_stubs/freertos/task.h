#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
