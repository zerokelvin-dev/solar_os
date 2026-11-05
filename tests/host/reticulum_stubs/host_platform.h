#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deletions of a task other than the caller, which never take effect. */
unsigned host_task_deletes_of_other_tasks(void);
/* Whether the caller is a task created through the stubs. */
bool host_task_is_current(void);

#ifdef __cplusplus
}
#endif
