#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#ifdef PASSPORT_LVGL_HEAP
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#include "ui_pool.h"
#define LV_MEM_SIZE (22 * 1024U)
#define LV_MEM_POOL_INCLUDE "ui_pool.h"
#define LV_MEM_POOL_ALLOC passport_ui_pool_alloc
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (32 * 1024U)
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130
// Montserrat 14/20/24/48 are RLE-compressed copies (montserrat_*.c, generate_passport_font.py).
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(passport_ui_font) LV_FONT_DECLARE(lv_font_montserrat_14) \
    LV_FONT_DECLARE(lv_font_montserrat_20) LV_FONT_DECLARE(lv_font_montserrat_24) LV_FONT_DECLARE(lv_font_montserrat_48)
#define LV_FONT_DEFAULT &passport_ui_font
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();
#define LV_USE_LOG 0
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_PNG 0
#define LV_USE_LODEPNG 0
#define LV_USE_JPEG_TURBO 0
#define LV_USE_GIF 0
#define LV_USE_FREETYPE 0
#define LV_USE_LOTTIE 0
#define LV_USE_ANIMIMG 0
#define LV_USE_SHADOW_CACHE 0
#define LV_USE_QRCODE 1
#endif
