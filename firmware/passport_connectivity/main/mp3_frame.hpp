#pragma once
#include <cstdint>

namespace passport {
// Offset of the first MPEG audio frame sync; a trailing 0xFF is kept since it may start one.
inline uint32_t mp3Sync(const uint8_t* data,uint32_t length){
    for(uint32_t i=0;i+1<length;++i)if(data[i]==0xff&&(data[i+1]&0xe0)==0xe0)return i;
    return length&&data[length-1]==0xff?length-1:length;
}
// Bytes in the Layer III frame whose 4-byte header starts at h, or 0 for an invalid header.
// Free-bitrate frames have no fixed size and are rejected.
inline uint32_t mp3FrameSize(const uint8_t* h){
    if(h[0]!=0xff||(h[1]&0xe0)!=0xe0)return 0;
    const unsigned version=h[1]>>3&3,layer=h[1]>>1&3,bitrate=h[2]>>4,rate=h[2]>>2&3;
    if(version==1||layer!=1||!bitrate||bitrate==15||rate==3)return 0;
    static constexpr uint16_t kbps[2][15]{{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
    static constexpr uint32_t hz[3]{44100,48000,32000};
    const bool mpeg1=version==3;
    const uint32_t sampleRate=hz[rate]>>(mpeg1?0:version==2?1:2);
    return (mpeg1?144U:72U)*kbps[!mpeg1][bitrate]*1000/sampleRate+(h[2]>>1&1);
}
}
