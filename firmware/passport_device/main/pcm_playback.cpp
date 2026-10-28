#include "pcm_playback.hpp"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
// Chat's TTS buffers come from the codec allocator: while Chat runs it has room beside the decoder (codec_memory.cpp).
extern "C" void* media_lib_module_malloc(const char*,size_t);
extern "C" void media_lib_free(void*);
namespace passport::board {
bool PcmPlayback::start(){
    stop();
    // PPP leaves enough total RAM but not a contiguous 9.6KiB block.
    for(auto& block:blocks_){block=static_cast<int16_t*>(media_lib_module_malloc("pcm",kBlock*sizeof(int16_t)));if(!block){stop();return false;}}
    available_=0;writeIndex_=readIndex_=0;
    finish_=false;done_=false;started_=false;firstUs_=missing_=tail_=0;
    samples_=gaps_=maxGap_=wallMs_=0;active_=true;return true;
}
bool PcmPlayback::push(const int16_t* pcm,size_t count){
    if(!active_||finish_||done_||(!pcm&&count)||count>kCapacity)return false;
    const auto until=esp_timer_get_time()+200000;
    while(kCapacity-available_.load(std::memory_order_acquire)<count){
        if(!active_||finish_||done_||esp_timer_get_time()>=until)return false;
        vTaskDelay(std::max<TickType_t>(1,pdMS_TO_TICKS(2)));
    }
    const auto size=count;
    while(count){const auto n=std::min(count,size_t(kBlock-writeIndex_%kBlock));memcpy(blocks_[writeIndex_/kBlock]+writeIndex_%kBlock,pcm,n*sizeof(*pcm));writeIndex_=(writeIndex_+n)%kCapacity;pcm+=n;count-=n;}
    available_.fetch_add(size,std::memory_order_release);return true;
}
size_t PcmPlayback::fillFromISR(int16_t* pcm,size_t count,BaseType_t* wake){
    (void)wake;
    memset(pcm,0,count*sizeof(*pcm));size_t samples=0;
    portENTER_CRITICAL_ISR(&mux_);
    if(active_&&!done_){
        const bool finished=finish_.load(std::memory_order_acquire);
        const auto available=available_.load(std::memory_order_acquire);
        if(!started_&&(available>=3840||finished)){started_=true;firstUs_=unsigned(esp_timer_get_time());}
        if(started_){
            samples=std::min(count,size_t(available));auto remaining=samples;auto* target=pcm;
            while(remaining){const auto n=std::min(remaining,size_t(kBlock-readIndex_%kBlock));memcpy(target,blocks_[readIndex_/kBlock]+readIndex_%kBlock,n*sizeof(*pcm));readIndex_=(readIndex_+n)%kCapacity;target+=n;remaining-=n;}
            available_.fetch_sub(samples,std::memory_order_release);
            samples_+=samples;
            if(samples){tail_=4;missing_=0;}
            if(samples<count){
                if(!finished){if(!missing_)++gaps_;missing_+=count-samples;maxGap_=std::max(maxGap_.load(),unsigned(uint64_t(missing_)*1000000/16000));}
                else if(!tail_||!--tail_){wallMs_=(unsigned(esp_timer_get_time())-firstUs_)/1000;done_=true;}
            }
        }
    }
    portEXIT_CRITICAL_ISR(&mux_);return samples;
}
bool PcmPlayback::stop(){
    int16_t* old[kBlocks];portENTER_CRITICAL(&mux_);active_=false;done_=true;
    for(unsigned i=0;i<kBlocks;++i){old[i]=blocks_[i];blocks_[i]=nullptr;}
    portEXIT_CRITICAL(&mux_);for(auto* block:old)if(block)media_lib_free(block);
    return true;
}
void PcmPlayback::snapshot(char* output,size_t capacity)const{
    snprintf(output,capacity,R"({"active":%s,"drained":%s,"samples":%u,"gaps":%u,"max_gap_us":%u,"wall_ms":%u,"audio_ms":%u,"dma":true})",active()?"true":"false",drained()?"true":"false",samples_.load(),gaps_.load(),maxGap_.load(),wallMs_.load(),samples_.load()/16);
}
}
