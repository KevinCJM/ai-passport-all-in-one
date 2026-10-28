#pragma once
#include <cstddef>
#include <cstring>

namespace passport {
// Append to a bounded response without a second JSON tree or heap buffer.
class JsonOutput {
    char* data_;size_t capacity_,used_;bool valid_;
public:
    JsonOutput(char* data,size_t capacity,size_t used=0):data_(data),capacity_(capacity),used_(used),valid_(used<capacity){if(valid_)data_[used_]=0;}
    bool append(const char* text){
        if(!valid_)return false;
        const size_t length=std::strlen(text);
        if(length>=capacity_-used_){valid_=false;return false;}
        std::memcpy(data_+used_,text,length+1);used_+=length;return true;
    }
    void string(const char* text){
        append("\"");
        for(const auto* p=reinterpret_cast<const unsigned char*>(text);valid_&&*p;++p){
            if(*p=='"')append("\\\"");
            else if(*p=='\\')append("\\\\");
            else if(*p<32){const char hex[]="0123456789abcdef";char escaped[]{'\\','u','0','0',hex[*p>>4],hex[*p&15],0};append(escaped);}
            else{const char byte[]{static_cast<char>(*p),0};append(byte);}
        }
        append("\"");
    }
    bool valid()const{return valid_;}
};
}
