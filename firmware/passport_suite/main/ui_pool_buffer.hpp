#pragma once
#include "apps.hpp"
#include "esp_heap_caps.h"
#include <cstddef>

namespace passport {
constexpr size_t kPagePool=25*1024,kChatPool=12*1024;
constexpr size_t uiPoolCapacity(App app){return app==App::Chat?kChatPool:kPagePool;}

// LVGL's pool. Every page shares one block that is allocated once and never freed, so BLE and the network
// cannot fragment it. Chat draws from a small block of its own and meanwhile lends the page block to the
// audio codecs: the Opus encoder needs 24KiB in one piece, which the shared heap no longer has beside the
// chat task's 32KiB stack once TLS is up.
class UiPoolBuffer {
    void* page_{};
    void* chat_{};
    static void* allocate(size_t size){return heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);}
public:
    UiPoolBuffer()=default;
    UiPoolBuffer(const UiPoolBuffer&)=delete;
    UiPoolBuffer& operator=(const UiPoolBuffer&)=delete;
    ~UiPoolBuffer(){reset();if(page_)heap_caps_free(page_);}
    // Releases what can be given back while waiting for other resources; the page block is kept.
    void reset(){if(chat_)heap_caps_free(chat_);chat_=nullptr;}
    bool prepare(size_t size){
        if(size==kChatPool)return chat_||(chat_=allocate(size));
        if(size!=kPagePool)return false;
        reset();
        return page_||(page_=allocate(size));
    }
    void* data(size_t size)const{return size==kChatPool?chat_:size==kPagePool?page_:nullptr;}
};
}
