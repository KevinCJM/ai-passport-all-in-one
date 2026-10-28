#pragma once
#include "suite.hpp"
#include "freertos/semphr.h"
#include "voice_gate.hpp"
#include <cstdlib>

namespace passport {
// UART1 is the simulator's microphone/speaker boundary, not a second ESP32.
class AudioPipe {
    App mode_{App::Home};
    uint32_t generation_{};
    TaskHandle_t task_{};
    SemaphoreHandle_t stopped_{};
    std::atomic<bool> stopping_{}, ready_{};
    void *encoder_{}, *decoder_{};
    uint32_t decoderRate_{};
    uint8_t *input_{}, *output_{};
    size_t pending_{},inputSize_{4096},outputSize_{6149};
#ifdef PASSPORT_DEVICE
    int listener_{-1},socket_{-1};bool deviceRecording_{};
    bool boardOpen_{};  // stop() closes the codec only when start() opened it: the 声音 page opens its own
    uint8_t diagnosticFrames_{};
    bool native_{};
    using NativeOutput=void(*)(void*,uint8_t,const uint8_t*,size_t);
    NativeOutput nativeOutput_{};void* nativeContext_{};
    void deviceLoop();
#endif
    struct VoiceState {
        VoiceGate *gate{};VoiceGate2k* fastGate{};
        int16_t (*ring)[480]{};unsigned slots;
        explicit VoiceState(unsigned frames):slots(frames){ring=static_cast<int16_t(*)[480]>(std::calloc(frames,sizeof(*ring)));}
        void clearGate(){delete gate;gate=nullptr;delete fastGate;fastGate=nullptr;}
        ~VoiceState(){clearGate();std::free(ring);}
        unsigned consecutive{};
        unsigned prerollBoost{1};

        uint8_t head{}, count{}, silence{};
        bool armed{}, pending{}, recording{};
    } *voice_{};
    size_t memoryReserve()const;
    bool encode(const uint8_t* data,size_t length);
    bool openOpusDecoder(uint32_t rate);
    void prepareFailure(unsigned stage,bool count=true);
    void voice(size_t length,const uint8_t* source=nullptr);
    void flushVoice();
    static void run(void* argument);
    bool read(uint8_t* data,size_t length);
    bool send(uint8_t type,const uint8_t* data,size_t length);
    void process(uint8_t type,size_t length);
public:
    std::atomic<uint32_t> encoded{}, decoded{}, pcm{}, rejected{}, errors{}, maximumUs{}, stackFree{}, interruptions{}, voiceEnds{}, voiceOverflows{};
    std::atomic<unsigned> vadMicRms{},vadRefRms{},vadResidualRms{};
    std::atomic<unsigned> gateFrames{},gatePeakMic{},gatePeakRef{},gatePeakResidual{},gateWarmFrames{},gateFloor{};
    std::atomic<unsigned> gateVeto[6]{};
    std::atomic<int> decoderResult{};
    std::atomic<unsigned> prepareError{},prepareFree{},prepareLargest{};
    std::atomic<int> encoderResult{};
    std::atomic<unsigned> encoderFree{};
    bool start(App app,uint32_t generation,bool native=false,uint32_t nativeRate=48000);
#ifdef PASSPORT_DEVICE
    void nativeOutput(NativeOutput callback,void* context){nativeOutput_=callback;nativeContext_=context;}
    bool nativeInput(uint8_t type,const uint8_t* data,size_t length);
    void nativePump();
    void gateSnapshot(char*,size_t)const;
#endif
    bool stop();
    bool active() const { return ready_; }
};
}
