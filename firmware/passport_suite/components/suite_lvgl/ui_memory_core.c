// Keep the pinned LVGL allocator unchanged. Only its initial pool size varies
// with the foreground page; TLSF's separate implementation keeps the constant
// maximum from lv_conf.h for its compile-time indexing tables.
#include "lvgl.h"
#include "ui_pool.h"
#undef LV_MEM_SIZE
#define LV_MEM_SIZE passport_ui_pool_size()
#include "src/stdlib/builtin/lv_mem_core_builtin.c"
