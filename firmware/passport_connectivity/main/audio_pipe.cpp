#include "audio_pipe.hpp"
#include "mp3_frame.hpp"
#ifdef PASSPORT_DEVICE
#include "board.hpp"
#include "lwip/sockets.h"
#include <fcntl.h>
#include <cerrno>
#endif
#include "ble_dictation.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <cstdio>
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_mp3_dec.h"

namespace passport {
namespace {
constexpr auto kPort=UART_NUM_1;
constexpr size_t kBuffer=4096, kDecoded=6144;
#ifdef PASSPORT_DEVICE
// The simulator's 48KiB reserve excludes physical I2S/DMA and TCP buffers.
// Keep a bounded reserve after each real codec allocation on the C3 target.
constexpr size_t kAudioReserve=16*1024;
#else
constexpr size_t kAudioReserve=kReserve;
#endif
uint32_t load32(const uint8_t* p) { return p[0]|(p[1]<<8U)|(p[2]<<16U)|(static_cast<uint32_t>(p[3])<<24U); }
void store32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=(v>>(i*8))&255; }
}
size_t AudioPipe::memoryReserve()const {
#ifdef PASSPORT_DEVICE
    // Native audio shares the network worker stack and has no host audio TCP buffers.
    return native_?8*1024:kAudioReserve;
#else
    return kAudioReserve;
#endif
}
bool AudioPipe::start(App app,uint32_t generation,bool native,uint32_t nativeRate) {
    if(!stop()) return false;
    if(app!=App::Chat && app!=App::Dictation && app!=App::Radio) return true;
    if(!stopped_) stopped_=xSemaphoreCreateBinary();
    if(!stopped_) return false;
    mode_=app; generation_=generation; pending_=0; stopping_=false;encoderResult=0;encoderFree=0;
    gateFrames=gatePeakMic=gatePeakRef=gatePeakResidual=gateWarmFrames=gateFloor=prepareError=prepareFree=prepareLargest=0;for(auto& count:gateVeto)count=0;
    // Allocate pre-roll only during playback; manual recording needs encoder RAM.
#ifdef PASSPORT_DEVICE
    native_=native;
    inputSize_=native&&app==App::Chat?2048:kBuffer;
    outputSize_=native&&app==App::Chat?2048:kDecoded+5;
    (void)nativeRate;
    const bool needsSocket=app!=App::Dictation||board::usbConnected();
    if(app==App::Dictation){inputSize_=256;outputSize_=256;dictationBle.usbOutput([](void* context,uint8_t type,const uint8_t* data,size_t size){return static_cast<AudioPipe*>(context)->send(type,data,size);},this);}
#else
    (void)native;(void)nativeRate;
    const bool needsSocket=true;
#endif
    if(needsSocket){
    input_=static_cast<uint8_t*>(heap_caps_malloc(inputSize_,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    output_=static_cast<uint8_t*>(heap_caps_malloc(outputSize_,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(!input_||!output_) { stop(); return false; }
    }
    #ifdef PASSPORT_DEVICE
    if(mode_==App::Dictation)dictationBle.usbConnected(false);
    listener_=needsSocket&&!native_?board::audioListen():-1;if((needsSocket&&!native_&&listener_<0)||!board::audioOpen(app!=App::Radio,app==App::Chat&&!native_?3:2,app==App::Chat&&!native_)){stop();return false;}
    boardOpen_=true;
    deviceRecording_=false;
    #else
    uart_config_t config{}; config.baud_rate=921600; config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE; config.stop_bits=UART_STOP_BITS_1; config.source_clk=UART_SCLK_DEFAULT;
    if(uart_param_config(kPort,&config)!=ESP_OK || uart_driver_install(kPort,2048,2048,0,nullptr,0)!=ESP_OK) { stop(); return false; }
    uart_flush_input(kPort);
    #endif
    if(app==App::Radio && esp_mp3_dec_open(nullptr,0,&decoder_)!=ESP_AUDIO_ERR_OK) { stop(); return false; }
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<memoryReserve()) { stop(); return false; }
#ifdef PASSPORT_DEVICE
    if(native_){ready_=true;return true;}
#endif
    // Measured SILK stack use exceeds 23KiB; keep over 4KiB measured margin.
    #ifdef PASSPORT_DEVICE
    // Physical Opus measured 27.5KiB peak stack; retain over 4KiB margin.
    const unsigned stack=app==App::Chat?32*1024:app==App::Dictation?8*1024:12*1024;
    #else
    const unsigned stack=app==App::Chat ? 28*1024 : 6*1024;
    #endif
    if(xTaskCreate(run,"live_audio",stack,this,4,&task_)!=pdPASS) { stop(); return false; }
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<memoryReserve()) { stop(); return false; }
    ready_=true; return true;
}
bool AudioPipe::stop() {
    if(task_) {
        stopping_=true;
        if(xSemaphoreTake(stopped_,pdMS_TO_TICKS(3000))!=pdTRUE) return false;
        task_=nullptr;
    }
    if(encoder_) { esp_opus_enc_close(encoder_); encoder_=nullptr; }
    if(decoder_) {
        if(mode_==App::Radio) esp_mp3_dec_close(decoder_); else esp_opus_dec_close(decoder_);
        decoder_=nullptr; decoderRate_=0;
    }
    #ifdef PASSPORT_DEVICE
    if(mode_==App::Dictation)dictationBle.usbConnected(false);
    if(socket_>=0){close(socket_);socket_=-1;}if(listener_>=0){close(listener_);listener_=-1;}if(boardOpen_){board::audioClose();boardOpen_=false;}
    #else
    if(uart_is_driver_installed(kPort)) uart_driver_delete(kPort);
    #endif
    if(input_) heap_caps_free(input_);
    if(output_) heap_caps_free(output_);
    input_=output_=nullptr; delete voice_; voice_=nullptr; pending_=0; ready_=false; return true;
}
bool AudioPipe::read(uint8_t* data,size_t length) {
    size_t used=0;
    while(used<length && !stopping_) {
        const int count=uart_read_bytes(kPort,data+used,length-used,pdMS_TO_TICKS(20));
        if(count>0) used+=count;
    }
    return used==length;
}
bool AudioPipe::send(uint8_t type,const uint8_t* data,size_t length) {
    uint8_t header[9]; header[0]=type; store32(header+1,generation_); store32(header+5,length);
    #ifdef PASSPORT_DEVICE
    if(type==2&&length>5){const unsigned rate=load32(data);if(!board::speaker(reinterpret_cast<const int16_t*>(data+5),(length-5)/2,rate,data[4]))++errors;}
    if(native_){if(type!=2&&nativeOutput_)nativeOutput_(nativeContext_,type,data,length);return true;}
    if(socket_>=0){
        // Physical speaker consumes PCM locally; retain only a small real PCM sample for diagnostics.
        if(type==2&&length>261){length=261;store32(header+5,length);}
        const auto writeAll=[&](const uint8_t* bytes,size_t size){while(size&&!stopping_){const int n=::send(socket_,bytes,size,0);if(n<=0){++errors;return false;}bytes+=n;size-=n;}return size==0;};
        return writeAll(header,sizeof(header))&&(!length||writeAll(data,length));
    }
    return false;
    #else
    return uart_write_bytes(kPort,header,sizeof(header))==static_cast<int>(sizeof(header))&&(!length||uart_write_bytes(kPort,data,length)==static_cast<int>(length));
    #endif
}
bool AudioPipe::encode(const uint8_t* data,size_t length) {
        if(!encoder_) {
            esp_opus_enc_config_t opus=ESP_OPUS_ENC_CONFIG_DEFAULT(); opus.sample_rate=16000; opus.channel=1;
            opus.bitrate=24000; opus.frame_duration=ESP_OPUS_ENC_FRAME_DURATION_60_MS; opus.complexity=0; opus.enable_vbr=true;opus.enable_dtx=true;
            encoderResult=esp_opus_enc_open(&opus,sizeof(opus),&encoder_);encoderFree=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
            if(encoderResult!=ESP_AUDIO_ERR_OK) { ++errors; return false; }
            if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<memoryReserve()) {
                printf("AUDIO_MEMORY_REJECT encoder free=%u\n",unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
                encoderResult=-1000;esp_opus_enc_close(encoder_); encoder_=nullptr; ++errors; return false;
            }
        }
        esp_audio_enc_in_frame_t raw{}; raw.buffer=const_cast<uint8_t*>(data); raw.len=length;
        esp_audio_enc_out_frame_t output{}; output.buffer=output_; output.len=std::min(outputSize_,kBuffer);
        if(esp_opus_enc_process(encoder_,&raw,&output)==ESP_AUDIO_ERR_OK) { send(1,output_,output.encoded_bytes); ++encoded; }
        else { ++errors;return false; }
        return true;

}
void AudioPipe::flushVoice() {
    while(voice_->count) {
        auto *samples=voice_->ring[voice_->head];
        #ifdef PASSPORT_DEVICE
        int16_t upsampled[960];
        #else
        auto *upsampled=reinterpret_cast<int16_t*>(input_);
        #endif
        for(int n=0;n<480;++n) upsampled[n*2]=upsampled[n*2+1]=std::clamp(int(samples[n])*int(voice_->prerollBoost),-32768,32767);
        if(!encode(reinterpret_cast<const uint8_t*>(upsampled),1920)) return;
        voice_->head=(voice_->head+1)%voice_->slots; --voice_->count;
    }
    voice_->prerollBoost=1;
}
void AudioPipe::voice(size_t length,const uint8_t* source) {
    if(!voice_ || length!=3840 || (!voice_->armed&&!voice_->recording&&!voice_->pending)) { ++rejected; return; }
    if(voice_->count==voice_->slots) {
        // Never overwrite the first syllable while the host acknowledges abort.
        ++voiceOverflows; ++errors; voice_->armed=voice_->pending=voice_->recording=false; return;
    }
    auto *clean=voice_->ring[(voice_->head+voice_->count)%voice_->slots];
    int16_t mic[160],ref[160]; bool speech=false;
    const auto *samples=reinterpret_cast<const int16_t*>(source?source:input_);
    for(int frame=0;frame<3;++frame) {
        for(int n=0;n<160;++n) {
            const int offset=frame*320+n*2;
            mic[n]=(static_cast<int32_t>(samples[offset])+samples[offset+1])/2;
            ref[n]=(static_cast<int32_t>(samples[960+offset])+samples[961+offset])/2;
        }
        if(voice_->fastGate){
            // Detection uses a 2kHz echo model (80ms path) on the single C3
            // core. Keep the microphone's higher band in the saved opening.
            int16_t downMic[40],downRef[40],downClean[40];
            for(unsigned n=0;n<40;++n){int m=0,r=0;for(unsigned j=0;j<4;++j){m+=mic[4*n+j];r+=ref[4*n+j];}downMic[n]=m/4;downRef[n]=r/4;}
            const bool detected=voice_->fastGate->process(downRef,downMic,downClean)>0;
            ++gateFrames;
            gatePeakMic=std::max<unsigned>(gatePeakMic.load(),voice_->fastGate->mic_power);
            gatePeakRef=std::max<unsigned>(gatePeakRef.load(),voice_->fastGate->reference_power);
            gatePeakResidual=std::max<unsigned>(gatePeakResidual.load(),voice_->fastGate->residual_power);
            gateWarmFrames=voice_->fastGate->warm_frames;
            gateFloor=voice_->fastGate->echo_floor;
            for(unsigned i=0;i<6;++i)if(voice_->fastGate->blocked&(1U<<i))++gateVeto[i];
            if(detected){uint64_t micEnergy=0,refEnergy=0;for(unsigned n=0;n<40;++n){micEnergy+=static_cast<int32_t>(downMic[n])*downMic[n];refEnergy+=static_cast<int32_t>(downRef[n])*downRef[n];}vadMicRms=std::sqrt(double(micEnergy)/40);vadRefRms=std::sqrt(double(refEnergy)/40);vadResidualRms=16*std::sqrt(voice_->fastGate->residual_power);}
            speech=detected||speech;
            for(unsigned n=0;n<160;++n)clean[frame*160+n]=std::clamp(int(mic[n])-(downMic[n/4]-downClean[n/4]),-32768,32767);
        }else if(voice_->gate) speech=voice_->gate->process(ref,mic,clean+frame*160)>0 || speech;
        else {
            // After abort, the speaker is stopped; exchange AEC memory for the encoder.
            uint64_t energy=0;unsigned crossings=0;
            for(int n=0;n<160;++n) { clean[frame*160+n]=mic[n];energy+=static_cast<int32_t>(mic[n])*mic[n];if(n&&((mic[n]<0)!=(mic[n-1]<0)))++crossings; }
            const bool active=energy/160>=640*640&&crossings>=2&&crossings<=60;
            voice_->consecutive=active?voice_->consecutive+1:0;
            speech=speech||voice_->consecutive>=3;
        }
    }
    ++voice_->count;
    if(voice_->recording) {
        flushVoice(); voice_->silence=speech?0:voice_->silence+1;
        if(voice_->silence>=12) {
            voice_->recording=false; voice_->silence=0;
            if(encoder_) { esp_opus_enc_close(encoder_); encoder_=nullptr; }
            ++voiceEnds; send(7,nullptr,0);
        }
    } else if(voice_->armed && speech) {
        voice_->armed=false; voice_->pending=true;
        if(decoder_) { esp_opus_dec_close(decoder_); decoder_=nullptr; decoderRate_=0; }
        #ifdef PASSPORT_DEVICE
        board::silence();
        // The speaker is muted now. Pending speech needs its PCM preserved,
        // not another costly echo-filter pass before the abort ACK can arrive.
        voice_->clearGate();voice_->consecutive=0;
        if(!board::playbackReference(false)){++errors;send(9,nullptr,0);return;}
        #endif
        ++interruptions; send(6,nullptr,0);
    } else if(!voice_->pending && voice_->count>3) { voice_->head=(voice_->head+1)%voice_->slots; --voice_->count; }
}
bool AudioPipe::openOpusDecoder(uint32_t rate){
    if(decoder_&&decoderRate_==rate)return true;
    if(decoder_)esp_opus_dec_close(decoder_);
    decoder_=nullptr;decoderRate_=0;
    esp_opus_dec_cfg_t config=ESP_OPUS_DEC_CONFIG_DEFAULT();config.sample_rate=rate;config.channel=1;
    decoderResult=esp_opus_dec_open(&config,sizeof(config),&decoder_);
    if(decoderResult!=ESP_AUDIO_ERR_OK){++errors;return false;}
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<memoryReserve()){
        esp_opus_dec_close(decoder_);decoder_=nullptr;decoderResult=-1000;++errors;return false;
    }
    decoderRate_=rate;return true;
}
void AudioPipe::prepareFailure(unsigned stage,bool count){
    prepareError=stage;prepareFree=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    prepareLargest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(count)++errors;
}
void AudioPipe::process(uint8_t type,size_t length) {
#ifdef PASSPORT_DEVICE
    if(mode_==App::Dictation&&type==12){if(!dictationBle.usbAck(input_,length))++rejected;return;}
    if(mode_==App::Chat&&type==8&&length==1){diagnosticFrames_=std::min<unsigned>(input_[0],20);return;}
#endif
    if(mode_==App::Chat && type==5 && length==1) {
        if(input_[0]==2)prepareError=0;
#ifdef PASSPORT_DEVICE
        if(native_){
            board::captureRecording(input_[0]==1);
            // Native chat is half duplex: no gate, pre-roll or playback capture.
            if(input_[0]==2){
                deviceRecording_=false;
                if(encoder_){esp_opus_enc_close(encoder_);encoder_=nullptr;}
                if(!openOpusDecoder(16000)){prepareFailure(1,false);return;}
                if(!board::bufferedSpeakerStart()){prepareFailure(3);return;}
                return;
            }
        }
#endif
        if(input_[0]==2&&!voice_){
#ifdef PASSPORT_DEVICE
            voice_=new(std::nothrow) VoiceState(native_?4:6);
#else
            voice_=new(std::nothrow) VoiceState(6);
#endif
            if(!voice_||!voice_->ring){delete voice_;voice_=nullptr;prepareFailure(2);return;}
        }
#ifdef PASSPORT_DEVICE
        if(input_[0]==2&&!board::playbackReference(true)){prepareFailure(4);return;}
        if((input_[0]==0||input_[0]==1)&&!board::playbackReference(false)){++errors;return;}
#endif
        if((input_[0]==0||(input_[0]==4&&voice_&&!voice_->pending&&!voice_->recording))&&voice_){
            voice_->armed=voice_->pending=voice_->recording=false;voice_->count=0;voice_->clearGate();
        }
        if(input_[0]==4){
#ifdef PASSPORT_DEVICE
            if(native_&&!board::bufferedSpeakerStop()){++errors;return;}
#endif
            if(decoder_&&(!voice_||(!voice_->pending&&!voice_->recording))){esp_opus_dec_close(decoder_);decoder_=nullptr;decoderRate_=0;}
            if(voice_&&!voice_->pending&&!voice_->recording){delete voice_;voice_=nullptr;
#ifdef PASSPORT_DEVICE
                if(!board::playbackReference(false)){++errors;return;}
#endif
            }
            return;
        }
#ifdef PASSPORT_DEVICE
        deviceRecording_=input_[0]==1;
        if(deviceRecording_&&voice_){voice_->clearGate();}
        if(input_[0]!=2)board::silence();
#endif
        if(input_[0]==2 && voice_) {
            // TTS start: begin echo adaptation and hold 180ms of microphone PCM.
#ifdef PASSPORT_DEVICE
            if(native_){
                if(!voice_->fastGate)voice_->fastGate=new(std::nothrow)VoiceGate2k;
                if(!voice_->fastGate){prepareFailure(5);return;}
                voice_->fastGate->reset();
            }else
#endif
            {
                if(!voice_->gate)voice_->gate=new(std::nothrow)VoiceGate;
                if(!voice_->gate){++errors;return;}
                voice_->gate->reset();
            }
            voice_->head=voice_->count=voice_->silence=0;
#ifdef PASSPORT_DEVICE
            voice_->prerollBoost=8;
#endif
            voice_->armed=true; voice_->pending=voice_->recording=false; return;
        }
        if(input_[0]==3 && voice_ && voice_->pending) {
            voice_->clearGate();voice_->consecutive=0;
            voice_->pending=false; voice_->recording=true; voice_->silence=0;
            flushVoice(); return;
        }
        if(input_[0]==1 && voice_) { delete voice_;voice_=nullptr; }
        if(input_[0]==1)encoderResult=0;
        // Recording and playback exchange codec state on this task, in order.
        if(input_[0]==0 && encoder_) { esp_opus_enc_close(encoder_); encoder_=nullptr; }
        if(input_[0]==1 && decoder_) { esp_opus_dec_close(decoder_); decoder_=nullptr; decoderRate_=0; }
        return;
    }
    if(mode_==App::Chat && type==6) { voice(length); return; }
    if(mode_==App::Dictation && type==3 && length && !(length%2)) {
        if(dictationBle.recording()) { if(!dictationBle.pcm(generation_,input_,length)) ++errors; }
        else if(dictationBle.phase()==0) send(3,input_,length); // Explicit audio-only diagnostic.
        ++pcm; return;
    }
    if(mode_==App::Chat && type==1 && length==1920) {
        encode(input_,length);
        return;
    }
    if(mode_==App::Chat && type==2 && length>4) {
#ifdef PASSPORT_DEVICE
        if(deviceRecording_){++rejected;return;}
#endif
        if(voice_ && (voice_->pending||voice_->recording)) { ++rejected; return; }
        auto rate=load32(input_);
        if(rate!=16000 && rate!=24000 && rate!=48000) { ++rejected; return; }
#ifdef PASSPORT_DEVICE
        if(native_)rate=16000; // Opus decodes directly to the board's I2S clock.
#endif
        if(!openOpusDecoder(rate))return;
        esp_audio_dec_in_raw_t raw{}; raw.buffer=input_+4; raw.len=length-4;
        esp_audio_dec_out_frame_t output{}; output.buffer=output_+5; output.len=outputSize_-5;
        esp_audio_dec_info_t info{};
        decoderResult=esp_opus_dec_decode(decoder_,&raw,&output,&info);
        if(decoderResult==ESP_AUDIO_ERR_OK) {
            store32(output_,rate); output_[4]=1; send(2,output_,output.decoded_size+5); ++decoded;
        } else ++errors;
        return;
    }
    if(mode_==App::Radio && type==4) {
        esp_audio_dec_in_raw_t raw{}; raw.buffer=input_; raw.len=length;
        for(unsigned frames=0; raw.len>=4 && frames<64 && !stopping_; ++frames) {
            // Streams start mid-frame and reads split frames. Give the decoder only whole frames (plus the next
            // header): a partial one fails after it has used the bit reservoir, and the frames after it fail too.
            const auto skip=mp3Sync(raw.buffer,raw.len);
            if(skip){raw.buffer+=skip;raw.len-=skip;continue;}
            const auto size=mp3FrameSize(raw.buffer);
            if(!size){++raw.buffer;--raw.len;continue;}
            if(size+4>raw.len) break;
            raw.consumed=0;
            esp_audio_dec_out_frame_t output{}; output.buffer=output_+5; output.len=outputSize_-5;
            esp_audio_dec_info_t info{};
            const auto rc=esp_mp3_dec_decode(decoder_,&raw,&output,&info);decoderResult=rc;
            if(raw.consumed>raw.len) { ++errors; break; }
            if(output.decoded_size && rc==ESP_AUDIO_ERR_OK) {
                store32(output_,info.sample_rate); output_[4]=info.channel; send(2,output_,output.decoded_size+5); ++decoded;
            }
            // A whole frame the decoder rejects is skipped; it keeps its own reservoir.
            if(!raw.consumed) raw.consumed=size;
            raw.buffer+=raw.consumed; raw.len-=raw.consumed;
        }
        pending_=std::min(static_cast<size_t>(raw.len),kBuffer/2);
        memmove(input_,raw.buffer,pending_); return;
    }
    ++rejected;
}
#ifdef PASSPORT_DEVICE
void AudioPipe::gateSnapshot(char* out,size_t size)const{
    snprintf(out,size,"{\"frames\":%u,\"peak_mic\":%u,\"peak_ref\":%u,\"peak_residual\":%u,\"warm_frames\":%u,\"floor\":%u,\"veto\":[%u,%u,%u,%u,%u,%u]}",gateFrames.load(),unsigned(16*std::sqrt(gatePeakMic.load())),unsigned(16*std::sqrt(gatePeakRef.load())),unsigned(16*std::sqrt(gatePeakResidual.load())),gateWarmFrames.load(),unsigned(16*std::sqrt(gateFloor.load())),gateVeto[0].load(),gateVeto[1].load(),gateVeto[2].load(),gateVeto[3].load(),gateVeto[4].load(),gateVeto[5].load());
}
bool AudioPipe::nativeInput(uint8_t type,const uint8_t* data,size_t length){
    if(!native_||!ready_||!input_||length>inputSize_-pending_){++rejected;return false;}
    if(length)memcpy(input_+pending_,data,length);
    const auto before=errors.load();process(type,pending_+length);stackFree=uxTaskGetStackHighWaterMark(nullptr);return errors.load()==before;
}
void AudioPipe::nativePump(){
    if(!native_||!ready_||mode_!=App::Chat||!deviceRecording_)return;
    struct CaptureFrame{int16_t mic[960],reference[960];} frame;
    // Interleave each captured frame with a socket read. Draining three frames
    // here can starve TTS decode for more than the playback buffer duration.
    for(unsigned n=0;n<1&&board::microphone(frame.mic,frame.reference);++n){
        encode(reinterpret_cast<const uint8_t*>(frame.mic),sizeof(frame.mic));
    }
    stackFree=uxTaskGetStackHighWaterMark(nullptr);
}
void AudioPipe::deviceLoop() {
    uint8_t header[9]{};
    size_t headerUsed = 0, payloadUsed = 0;
    uint32_t length = 0;
    char authentication[64];
    size_t authUsed = 0;
    bool authenticated = false;
    struct CaptureFrame {
        int16_t mic[960], reference[960];
    } frame;
    int64_t authDeadline = 0;
    if(listener_>=0)fcntl(listener_, F_SETFL, O_NONBLOCK);
    while (!stopping_) {
        if (socket_ < 0 && listener_>=0) {
            socket_ = accept(listener_, nullptr, nullptr);
            if (socket_ >= 0) {
                timeval timeout{0, 200000};
                setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                authUsed = 0;
                authenticated = false;
                authDeadline = esp_timer_get_time() + 3000000;
            }
        }
        if (socket_ >= 0 && !authenticated) {
            int n = recv(socket_, authentication + authUsed, sizeof(authentication) - authUsed,
                         MSG_DONTWAIT);
            if (n > 0)
                authUsed += n;
            if (n == 0 || esp_timer_get_time() > authDeadline) {
                close(socket_);
                socket_ = -1;
            } else if (authUsed == sizeof(authentication)) {
                unsigned diff = 0;
                for (unsigned i = 0; i < 64; ++i)
                    diff |= authentication[i] ^ board::apiToken()[i];
                if (diff) {
                    close(socket_);
                    socket_ = -1;
                } else {
                    authenticated = true;
                    if(mode_==App::Dictation){dictationBle.usbConnected(true);send(13,nullptr,0);}
                    headerUsed = payloadUsed = 0;
                }
            }
        }
        // After abort, discard queued TTS quickly so its control ACK reaches the
        // bounded first-syllable buffer before another microphone frame arrives.
        for(unsigned networkBudget=0;socket_>=0&&authenticated&&networkBudget<(voice_&&voice_->pending?16U:1U);++networkBudget) {
            uint8_t *destination =
                headerUsed < 9 ? header + headerUsed : input_ + pending_ + payloadUsed;
            size_t wanted = headerUsed < 9 ? 9 - headerUsed : length - payloadUsed;
            const int n = wanted ? recv(socket_, destination, wanted, MSG_DONTWAIT) : -1;
            if (n == 0) {
                if(mode_==App::Dictation)dictationBle.usbConnected(false);
                close(socket_);
                socket_ = -1;
                headerUsed = payloadUsed = 0;
                deviceRecording_ = false;
                board::silence();
            } else if (n > 0) {
                if (headerUsed < 9) {
                    headerUsed += n;
                    if (headerUsed == 9) {
                        length = load32(header + 5);
                        payloadUsed = 0;
                        if (length > inputSize_ - pending_) {
                            ++rejected;
                            close(socket_);
                            socket_ = -1;
                            headerUsed = 0;
                        }
                    }
                } else
                    payloadUsed += n;
                if (socket_ >= 0 && headerUsed == 9 && payloadUsed == length) {
                    if (load32(header + 1) == generation_) {
                        const auto before = esp_timer_get_time();
                        process(header[0], pending_ + length);
                        const auto elapsed = esp_timer_get_time() - before;
                        if (elapsed > maximumUs)
                            maximumUs = elapsed;
                    } else
                        ++rejected;
                    headerUsed = payloadUsed = 0;
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if(mode_==App::Dictation)dictationBle.usbConnected(false);
                close(socket_);
                socket_ = -1;
                deviceRecording_ = false;
            }
        }
        // Capture uses a separate frame: a partial network payload remains untouched.
        for (unsigned budget = 0; budget < 3 && board::microphone(frame.mic, frame.reference);
             ++budget) {
            if (mode_ == App::Dictation) {
                if (dictationBle.recording() &&
                    !dictationBle.pcm(generation_, reinterpret_cast<const uint8_t *>(frame.mic),
                                      sizeof(frame.mic)))
                    ++errors;
            } else if (mode_ == App::Chat && authenticated && socket_ >= 0) {
                if (deviceRecording_)
                    encode(reinterpret_cast<const uint8_t *>(frame.mic), sizeof(frame.mic));
                else if (voice_ && (voice_->armed || voice_->pending || voice_->recording)) {
                    if(voice_->armed&&diagnosticFrames_){send(8,reinterpret_cast<const uint8_t*>(&frame),sizeof(frame));--diagnosticFrames_;}
                    voice(sizeof(frame), reinterpret_cast<const uint8_t *>(&frame));
                    if(voice_->pending)break;
                }
            }
        }
        stackFree = uxTaskGetStackHighWaterMark(nullptr);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif
void AudioPipe::run(void *argument) {
    auto &self = *static_cast<AudioPipe *>(argument);
#ifdef PASSPORT_DEVICE
    self.deviceLoop();
#else
    while (!self.stopping_) {
        uint8_t header[9];
        if (!self.read(header, sizeof(header)))
            break;
        const auto generation = load32(header + 1), length = load32(header + 5);
        if (length > kBuffer - self.pending_) {
            ++self.rejected;
            uart_flush_input(kPort);
            self.pending_ = 0;
            continue;
        }
        const auto offset = self.pending_;
        if (!self.read(self.input_ + offset, length))
            break;
        if (generation != self.generation_) {
            ++self.rejected;
            continue;
        }
        const auto start = esp_timer_get_time();
        self.process(header[0], offset + length);
        const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
        if (elapsed > self.maximumUs)
            self.maximumUs = elapsed;
        self.stackFree = uxTaskGetStackHighWaterMark(nullptr);
    }
#endif
    xSemaphoreGive(self.stopped_); vTaskDelete(nullptr);
}
}
