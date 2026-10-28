#pragma once
#include "freertos/FreeRTOS.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace passport::board {
// One task produces PCM; the TX DMA callback consumes one 10ms block.
class PcmPlayback {
    static constexpr unsigned kBlock=1600,kBlocks=3,kCapacity=kBlock*kBlocks;
    int16_t* blocks_[kBlocks]{};
    std::atomic<unsigned> available_{};
    unsigned writeIndex_{},readIndex_{};
    portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> finish_{},done_{true},active_{};
    std::atomic<unsigned> samples_{},gaps_{},maxGap_{},wallMs_{};
    bool started_{};unsigned firstUs_{},missing_{},tail_{};
public:
    bool start();
    bool push(const int16_t*,size_t);
    size_t fillFromISR(int16_t*,size_t,BaseType_t*);
    void finish(){finish_=true;}
    bool drained()const{return !active_.load()||done_.load();}
    bool active()const{return active_.load();}
    bool stop();
    void snapshot(char*,size_t)const;
};
}
