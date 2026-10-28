#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace passport::portal {
inline unsigned hexDigit(char ch){return ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:ch>='A'&&ch<='F'?ch-'A'+10:16;}
inline bool decodeSsid(const char* hex,char* output){
    if(!hex)return false;
    const size_t length=std::strlen(hex);
    if(!length||length>64||length%2)return false;
    for(size_t i=0;i<length;i+=2){const unsigned high=hexDigit(hex[i]),low=hexDigit(hex[i+1]);if(high>15||low>15||!(high|low))return false;output[i/2]=char(high*16+low);}
    output[length/2]=0;return true;
}
inline void encodeSsid(const uint8_t* input,char* output){
    const char* digits="0123456789abcdef";unsigned i=0;
    for(;i<32&&input[i];++i){output[2*i]=digits[input[i]>>4];output[2*i+1]=digits[input[i]&15];}
    output[2*i]=0;
}
inline bool passwordValid(const char* value){
    if(!value)return false;
    const size_t length=std::strlen(value);
    if(!length)return true;
    if(length>=8&&length<=63)return true;
    if(length!=64)return false;
    for(unsigned i=0;i<64;++i)if(hexDigit(value[i])>15)return false;
    return true;
}
// One bounded, uncompressed DNS question; no recursion or HTTPS interception.
inline size_t dnsReply(const uint8_t* query,size_t length,uint8_t* out,size_t capacity){
    if(!query||length<17||length>512||(query[2]&0xf8)||query[4]||query[5]!=1||query[6]||query[7]||query[8]||query[9])return 0;
    size_t end=12,total=0;
    while(end<length&&query[end]){const unsigned label=query[end++];if(label>63||end+label>=length||total+label+1>254)return 0;end+=label;total+=label+1;}
    if(end+5>length)return 0;
    ++end;const unsigned type=query[end]*256+query[end+1],klass=query[end+2]*256+query[end+3];end+=4;
    const bool answer=klass==1&&(type==1||type==255);
    const size_t size=end+(answer?16:0);if(size>capacity)return 0;
    std::memcpy(out,query,end);out[2]=0x81;out[3]=0x80;out[6]=0;out[7]=answer;out[8]=out[9]=out[10]=out[11]=0;
    if(answer){const uint8_t record[]={0xc0,0x0c,0,1,0,1,0,0,0,0,0,4,192,168,8,1};std::memcpy(out+end,record,sizeof(record));}
    return size;
}
}
