#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace passport {
// Each ATT value is bounded independently of the negotiated MTU.
struct BleExchange {
    static constexpr size_t capacity=2304,packetSize=180,headerSize=8;
    char request[capacity]{},response[capacity]{};
    uint32_t id{};uint16_t total{},received{},responseSize{},readOffset{};
    bool pending{},ready{};
    static uint32_t number(const uint8_t* p){return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
    static void put(uint8_t* p,uint32_t n,unsigned size){for(unsigned i=0;i<size;++i)p[i]=n>>(8*i);}
    static bool boundedJson(const char* text){
        unsigned depth=0;bool quoted=false,escaped=false;
        for(const char* p=text;*p;++p){
            if(quoted){if(escaped)escaped=false;else if(*p=='\\')escaped=true;else if(*p=='"')quoted=false;continue;}
            if(*p=='"')quoted=true;
            else if(*p=='{'||*p=='['){if(++depth>12)return false;}
            else if(*p=='}'||*p==']'){if(!depth)return false;--depth;}
        }
        return !quoted&&!depth;
    }
    void reset(){std::memset(request,0,sizeof(request));std::memset(response,0,sizeof(response));id=0;total=received=responseSize=readOffset=0;pending=ready=false;}
    bool accept(const uint8_t* bytes,size_t size){
        if(size<=headerSize||size>packetSize||pending)return false;
        const auto incoming=number(bytes);const unsigned offset=bytes[4]|unsigned(bytes[5])<<8,length=bytes[6]|unsigned(bytes[7])<<8;
        const size_t payload=size-headerSize;
        if(std::memchr(bytes+headerSize,0,payload))return false;
        if(!incoming||!length||length>=capacity||offset>length||payload>length-offset)return false;
        if(!offset){id=incoming;total=length;received=readOffset=responseSize=0;ready=false;request[0]=0;}
        if(id!=incoming||total!=length||received!=offset)return false;
        std::memcpy(request+received,bytes+headerSize,payload);received+=payload;request[received]=0;
        pending=received==total;return true;
    }
    void complete(){responseSize=std::strlen(response);readOffset=0;pending=false;ready=true;std::memset(request,0,sizeof(request));}
    size_t read(uint8_t* bytes){
        std::memset(bytes,0,headerSize);
        if(!ready)return headerSize;
        const size_t remaining=responseSize-readOffset,amount=remaining<packetSize-headerSize?remaining:packetSize-headerSize;
        put(bytes,id,4);put(bytes+4,readOffset,2);put(bytes+6,responseSize,2);
        std::memcpy(bytes+headerSize,response+readOffset,amount);readOffset+=amount;
        return headerSize+amount;
    }
};
}
