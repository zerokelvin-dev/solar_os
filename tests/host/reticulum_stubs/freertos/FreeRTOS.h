#pragma once

/* FreeRTOS on POSIX threads, for tests where tasks run concurrently. Time
 * runs ten times fast: a tick is a millisecond of FreeRTOS time and a tenth
 * of one on the host clock. */

#include <stdint.h>

typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint8_t StackType_t;
typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffU
#define tskIDLE_PRIORITY 0U
#define tskNO_AFFINITY 0x7fffffff
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
