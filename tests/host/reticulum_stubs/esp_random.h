#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);
void esp_fill_random(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif
