#pragma once
struct _lv_obj_t;
namespace passport {
class Runtime;
_lv_obj_t* statusBarInit(_lv_obj_t* screen);
void statusBarRender(const Runtime& runtime);
}
