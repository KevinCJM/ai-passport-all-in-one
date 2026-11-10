#pragma once
#include "suite.hpp"
#include "freertos/semphr.h"
#include "voice_gate.hpp"
#include <cstdlib>

namespace passport {
// UART1 is the simulator's microphone/speaker boundary, not a second ESP32.
// 语音指令: the end of a spoken phrase from the microphone alone (the cloud's own detection did not end a phrase
// whose start was sent late, in one go). A 60 ms frame is speech when clearly above the room's level (a floor that
// follows quieter frames down at once and non-speech louder ones up slowly); after some speech, 0.72 s of none ends the phrase.
struct PhraseEnd {
    unsigned floor{}, heard{}, quiet{}, peak{};
    static constexpr unsigned kSpeech = 3, kQuiet = 12, kMinimum = 300;
    void reset() { *this = {}; }
    bool frame(const int16_t* pcm, unsigned count) {
        uint64_t sum = 0;
        for (unsigned n = 0; n < count; ++n) sum += pcm[n] < 0 ? -pcm[n] : pcm[n];
        const unsigned level = count ? unsigned(sum / count) : 0;
        peak = level > peak ? level : peak;
        const bool speech = floor && level > floor * 3 && level > kMinimum;
        if (!floor || level < floor) floor = level ? level : 1; else if (!speech) floor += (level - floor) / 16;
        if (speech) { ++heard; quiet = 0; } else if (heard >= kSpeech) ++quiet;
        return heard >= kSpeech && quiet >= kQuiet;
    }
};
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
    unsigned encoderBitrate{24000};  // speech; 语音指令 uses less so its phrase fits while it connects (start resets it)
    bool start(App app,uint32_t generation,bool native=false,uint32_t nativeRate=48000);
#ifdef PASSPORT_DEVICE
    void nativeOutput(NativeOutput callback,void* context){nativeOutput_=callback;nativeContext_=context;}
    bool nativeInput(uint8_t type,const uint8_t* data,size_t length);
    void nativePump();
    PhraseEnd phrase;std::atomic<bool> phraseEnded{};  // 语音指令: fed by nativePump while recording
    void gateSnapshot(char*,size_t)const;
#endif
    bool stop();
    bool active() const { return ready_; }
};
}
