#pragma once
#include <cstddef>
#include <cstdint>

// Adapted from FoloToy/ai-passport, commit 1178e40713455754ec9a6a2dd8e5dde46f80e513.
// Copyright (c) 2026 FoloToy. MIT license: docs/licenses/FoloToy-MIT.txt.
namespace passport {
struct BatteryReading {
    bool present{}, known{};
    uint8_t percent{};
    uint16_t millivolts{};
};

template<class Bus> class Cw2017 {
    Bus& bus_;
    BatteryReading reading_{};
    bool initialized_{}, waiting_{};
    uint64_t next_{}, deadline_{};
    bool read(uint8_t reg,uint8_t* data,size_t size) { return bus_.read(reg,data,size); }
    bool mode(uint8_t value) {
        if(!bus_.write(0x08,0x30))return false;
        bus_.delayMs(20);
        if(!bus_.write(0x08,value))return false;
        bus_.delayMs(10);return true;
    }
public:
    // Official BSP cell profile, copied byte for byte; do not infer it from battery voltage.
    static constexpr uint8_t profile[80]={
        0x64,0,0,0,0,0,0,0,0xAD,0xC7,0xC8,0xCA,0xBD,0xB1,0xC1,0x94,
        0x88,0xD1,0xBD,0x97,0x88,0x66,0x56,0x4A,0x3F,0x33,0x26,0x5C,0x37,0xD1,0x27,0xD8,
        0xCC,0xB7,0xCF,0xB3,0xB2,0xAE,0xA6,0x9E,0x99,0x97,0x9B,0x86,0x47,0x1E,0x17,0x26,
        0x49,0x96,0xD9,0xE1,0xDD,0xDC,0xD4,0x59,0,0,0x90,2,0,0,0,0,
        0,0,0x64,0,0,0,0,0,0,0,0,0,0,0,0,0x5C
    };
    unsigned reads{}, errors{};
    bool profileUpdated{};
    explicit Cw2017(Bus& bus):bus_(bus){}
    BatteryReading reading() const { return reading_; }
    bool initialize(uint64_t now) {
        reading_={};initialized_=waiting_=profileUpdated=false;
        uint8_t version{},flag{};
        if(!read(0x00,&version,1))return false;
        reading_.present=true;
        if(!read(0x0b,&flag,1))return false;
        bool matches=(flag&0x80)!=0;
        if(matches)for(size_t i=0;i<sizeof(profile);++i){
            uint8_t value{};
            if(!read(0x10+i,&value,1))return false;
            if(value!=profile[i]){matches=false;break;}
        }
        if(!matches){
            if(!mode(0xf0))return false;
            for(size_t i=0;i<sizeof(profile);++i)if(!bus_.write(0x10+i,profile[i]))return false;
            for(size_t i=0;i<sizeof(profile);++i){
                uint8_t value{};if(!read(0x10+i,&value,1)||value!=profile[i])return false;
            }
            if(!read(0x0b,&flag,1)||!bus_.write(0x0b,flag|0x80)||!mode(0))return false;
            profileUpdated=true;
        }else{
            uint8_t config{};if(!read(0x08,&config,1)||(config!=0&&!mode(0)))return false;
        }
        initialized_=waiting_=true;next_=now+100;deadline_=now+5000;return true;
    }
    void poll(uint64_t now) {
        if(!initialized_||now<next_)return;
        ++reads;
        uint8_t soc[2]{},voltage[2]{};
        const bool valid=read(0x04,soc,2)&&soc[0]<=100&&read(0x02,voltage,2);
        const auto raw=static_cast<uint16_t>(((unsigned(voltage[0])<<8)|voltage[1])&0x3fff);
        const unsigned mv=uint32_t(raw)*3125/10000;
        reading_.known=valid&&mv>0&&mv<=5000;
        if(reading_.known){reading_.percent=soc[0];reading_.millivolts=mv;waiting_=false;}
        else {reading_.percent=0;reading_.millivolts=0;++errors;if(now>=deadline_)waiting_=false;}
        next_=now+(waiting_?100:10000);
    }
    bool sleep() {
        if(!reading_.present)return true;
        for(unsigned attempt=0;attempt<2;++attempt){
            const bool written=bus_.write(0x08,0xf0);bus_.delayMs(5);
            uint8_t config{};
            if(read(0x08,&config,1)&&written&&config==0xf0){initialized_=waiting_=false;reading_.known=false;return true;}
        }
        return false;
    }
};
} // namespace passport
