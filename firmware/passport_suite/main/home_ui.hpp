#pragma once
#include "suite.hpp"
struct _lv_obj_t;
namespace passport {
void homeUiInit(_lv_obj_t* screen);
void homeUiRender(const Runtime& runtime);
}
