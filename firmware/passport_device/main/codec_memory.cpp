// esp_audio_codec allocates through these weak hooks. While Chat runs, its codecs draw first from the UI
// page block it lends (passport_codec_arena, ui_pool_buffer.hpp); everything else uses the heap.
#include "multi_heap.h"
#include "freertos/FreeRTOS.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
std::atomic<multi_heap_handle_t> arena{};
const uint8_t *begin{},*end{};size_t whole{};
bool owned(const void* p){return arena.load()&&p>=begin&&p<end;}
// ponytail: temporary memory survey; remove with the /status?mem query.
std::atomic<unsigned> arenaPeak{},spilled{},spilledBytes{};
}
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <cstdio>
extern "C" int passport_memory_report(char* out,size_t size){
    int used=snprintf(out,size,"{\"arena_peak\":%u,\"arena_whole\":%u,\"spilled\":%u,\"spilled_bytes\":%u,\"free\":%u,\"largest\":%u,\"stacks\":{",arenaPeak.load(),unsigned(whole),spilled.load(),spilledBytes.load(),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    const char* names[]{"main","IDLE","Tmr Svc","esp_timer","sys_evt","tiT","suite_worker","device_mic","usb_ppp","httpd","native_online","nimble_host","btController","portal_dns","ppp_https","live_audio"};
    bool first=true;for(const char* n:names)if(auto t=xTaskGetHandle(n)){used+=snprintf(out+used,size-used,"%s\"%s\":%u",first?"":",",n,unsigned(uxTaskGetStackHighWaterMark(t)));first=false;}
    used+=snprintf(out+used,size-used,"}}");return used;
}
// Each task's share of the CPU since the last ask (since boot the first time), largest first, in tenths of a percent.
extern "C" int passport_cpu_report(char* out,size_t size){
    constexpr unsigned kMax=28;struct Seen{TaskHandle_t task;uint32_t time;};static Seen last[kMax];static uint32_t lastTotal=0;
    auto* tasks=static_cast<TaskStatus_t*>(malloc(sizeof(TaskStatus_t)*kMax));  // not on the httpd task's small stack
    if(!tasks)return snprintf(out,size,"{\"error\":\"memory\"}");
    uint32_t total=0;const unsigned n=uxTaskGetSystemState(tasks,kMax,&total);
    uint32_t spent[kMax];
    for(unsigned i=0;i<n;++i){spent[i]=tasks[i].ulRunTimeCounter;for(auto& l:last)if(l.task==tasks[i].xHandle){if(spent[i]>=l.time)spent[i]-=l.time;break;}}
    const uint32_t span=std::max<uint32_t>(1,total-lastTotal);
    int used=snprintf(out,size,"{\"window_ms\":%lu,\"tasks\":{",static_cast<unsigned long>(span/1000));
    bool first=true;
    for(unsigned k=0;k<n;++k){
        unsigned best=k;for(unsigned i=k+1;i<n;++i)if(spent[i]>spent[best])best=i;
        std::swap(spent[k],spent[best]);std::swap(tasks[k],tasks[best]);
        const unsigned tenths=static_cast<unsigned>(uint64_t(spent[k])*1000/span);
        if(tenths&&used<int(size)-48){used+=snprintf(out+used,size-used,"%s\"%s\":%u.%u",first?"":",",tasks[k].pcTaskName,tenths/10,tenths%10);first=false;}
    }
    for(unsigned i=0;i<kMax;++i)last[i]=i<n?Seen{tasks[i].xHandle,tasks[i].ulRunTimeCounter}:Seen{};
    lastTotal=total;free(tasks);
    return used+snprintf(out+used,size-used,"}}");
}
extern "C" bool passport_codec_arena(void* memory,size_t size){
    if(memory){
        auto handle=multi_heap_register(memory,size);if(!handle)return false;
        multi_heap_set_lock(handle,&lock);
        begin=static_cast<uint8_t*>(memory);end=begin+size;whole=multi_heap_free_size(handle);arena=handle;
        return true;
    }
    auto handle=arena.load();
    if(handle&&multi_heap_free_size(handle)!=whole)return false;
    arena=nullptr;return true;
}
extern "C" void* media_lib_module_malloc(const char*,size_t size){
    if(auto handle=arena.load()){
        if(void* p=multi_heap_malloc(handle,size)){const unsigned u=whole-multi_heap_free_size(handle);if(u>arenaPeak)arenaPeak=u;return p;}
        ++spilled;spilledBytes+=size;
    }
    return malloc(size);
}
extern "C" void* media_lib_module_calloc(const char* module,size_t n,size_t size){
    if(size&&n>SIZE_MAX/size)return nullptr;
    void* p=media_lib_module_malloc(module,n*size);if(p)memset(p,0,n*size);
    return p;
}
extern "C" void media_lib_free(void* p){
    if(owned(p))multi_heap_free(arena.load(),p);else free(p);
}
extern "C" void* media_lib_module_realloc(const char* module,void* p,size_t size){
    if(!owned(p))return p?realloc(p,size):media_lib_module_malloc(module,size);
    if(void* resized=multi_heap_realloc(arena.load(),p,size))return resized;
    void* moved=malloc(size);if(!moved)return nullptr;
    memcpy(moved,p,std::min(size,multi_heap_get_allocated_size(arena.load(),p)));multi_heap_free(arena.load(),p);
    return moved;
}
