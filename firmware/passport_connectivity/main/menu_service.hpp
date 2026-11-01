#pragma once
#include "suite.hpp"
#include "cJSON.h"
#include "freertos/semphr.h"
namespace passport {
// One staging slot, applied only by the UI/main loop. Never copy a layout into every command queue slot.
class MenuService {
    Runtime* runtime_{};
    StaticSemaphore_t storage_{};SemaphoreHandle_t mutex_{};
    MenuLayout pending_{};
    uint32_t revision_{},nextTicket_{},pendingTicket_{},doneTicket_{};
    int result_{};
public:
    void init(Runtime& runtime);
    int request(const cJSON* data,char* output,size_t size);
    static int catalog(unsigned group,char* output,size_t size);
    bool poll();
    uint32_t revision()const{return revision_;} // Main-loop only, for invalidating physical key contexts.
};
}
