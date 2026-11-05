#pragma once

/* compat.h for C and C++ alike: glibc before 2.38 has no strlcpy, which
 * host_platform.c then supplies. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlcpy(char *dst, const char *src, size_t size);

#ifdef __cplusplus
}
#endif
