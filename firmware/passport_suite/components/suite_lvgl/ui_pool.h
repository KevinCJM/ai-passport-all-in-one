#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t passport_ui_pool_size(void);
void* passport_ui_pool_alloc(size_t bytes);
#ifdef __cplusplus
}
#endif
