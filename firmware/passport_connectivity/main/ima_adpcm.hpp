#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace passport {
struct ImaState {int predictor{},index{};};
// Independent blocks carry the starting state and decoded sample count.
inline size_t imaBlock(const int16_t* pcm,size_t count,uint8_t* out,size_t capacity,ImaState& state){
    if(!count||count>310||capacity<5+(count+1)/2)return 0;
    static constexpr int steps[]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
    static constexpr int indices[]={-1,-1,-1,-1,2,4,6,8};
    out[0]=state.predictor&255;out[1]=(state.predictor>>8)&255;out[2]=state.index;out[3]=count&255;out[4]=count>>8;
    for(size_t i=0;i<count;++i){
        int difference=int(pcm[i])-state.predictor;unsigned code=difference<0?8:0;if(difference<0)difference=-difference;
        int step=steps[state.index],delta=step>>3;
        if(difference>=step){code|=4;difference-=step;delta+=step;}step>>=1;
        if(difference>=step){code|=2;difference-=step;delta+=step;}step>>=1;
        if(difference>=step){code|=1;delta+=step;}
        state.predictor=std::clamp(state.predictor+((code&8)?-delta:delta),-32768,32767);
        state.index=std::clamp(state.index+indices[code&7],0,88);
        if(!(i&1))out[5+i/2]=code<<4;else out[5+i/2]|=code;
    }
    return 5+(count+1)/2;
}
}
