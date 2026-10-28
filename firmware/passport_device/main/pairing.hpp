#pragma once
#include <cstdint>
#include <cstring>

namespace passport {
// The caller supplies a monotonic clock and serializes access.
class Pairing {
    char nonce_[33]{};
    uint64_t expires_{};
    bool approved_{};
public:
    bool active(uint64_t now) const { return *nonce_ && now < expires_; }
    bool begin(const char* nonce,uint64_t now) {
        if(!nonce || std::strlen(nonce)!=32 || active(now))return false;
        for(unsigned i=0;i<32;++i)if(!((nonce[i]>='0'&&nonce[i]<='9')||(nonce[i]>='a'&&nonce[i]<='f')))return false;
        std::memcpy(nonce_,nonce,33);expires_=now+60;approved_=false;return true;
    }
    void key(bool allow,uint64_t now) {if(active(now)){if(allow){approved_=true;expires_=now+15;}else cancel();}}
    bool code(char* out,uint64_t now) const {out[0]=0;if(!active(now)||approved_)return false;std::memcpy(out,nonce_,6);out[6]=0;return true;}
    int poll(const char* nonce,uint64_t now) {
        if(!active(now)||!nonce||std::strlen(nonce)!=32)return -1;
        unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=nonce[i]^nonce_[i];if(diff)return -1;
        if(!approved_)return 0;
        cancel();return 1;
    }
    void cancel(){nonce_[0]=0;approved_=false;expires_=0;}
};
}
