#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "../../passport_suite/main/radio_stations.hpp"
#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace passport {
// City in China, chosen by hand on the setup page (never located by IP), for weather and local stations.
// topics and keyword are unused; they keep the layout of preferences saved by older firmware.
struct MediaPrefs{static constexpr uint32_t kVersion=2;uint32_t version{kVersion};char city[48]{};float latitude{},longitude{};char country[4]{};uint32_t topics{};char keyword[48]{};char province[24]{};};

// Stations for a city: those named after it, then the rest of its province, then national ones.
// Municipalities and Hong Kong have no province and are a region of their own.
constexpr unsigned kMaxRadios=24,kMaxRegional=18;
enum class RadioTier:uint8_t{Local,Province,National};
struct RadioPicks{uint16_t index[kMaxRadios]{};uint8_t count{};int region{-1};};
inline RadioPicks pickRadios(const char* city,const char* province){
    RadioPicks picks;const char* area=*province?province:city;
    for(unsigned i=1;i<std::size(kRadioRegions)&&*area;++i)if(!strcmp(kRadioRegions[i],area))picks.region=static_cast<int>(i);
    const auto add=[&](unsigned i,unsigned limit){
        for(unsigned j=0;j<picks.count;++j)if(picks.index[j]==i)return;
        if(picks.count<limit)picks.index[picks.count++]=static_cast<uint16_t>(i);
    };
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<std::size(kRadioStations);++i){
        const auto& s=kRadioStations[i];
        if(static_cast<int>(s.region)==picks.region&&(pass||strstr(s.name,city)))add(i,kMaxRegional);
    }
    for(unsigned i=0;i<std::size(kRadioStations);++i)if(!kRadioStations[i].region)add(i,kMaxRadios);
    return picks;
}
inline RadioTier radioTier(const RadioStation& s,const char* city,const char* province){
    return !s.region?RadioTier::National:!*province||strstr(s.name,city)?RadioTier::Local:RadioTier::Province;
}
// Plain HTTP for Qingting's public relays: a TLS session's buffers do not fit beside the MP3 decoder.
inline void radioUrl(const RadioStation& s,char* out,size_t capacity,bool backup=false){
    if(s.url)snprintf(out,capacity,"%s",s.url);
    else snprintf(out,capacity,"http://%s/live/%u/64k.mp3",backup?"lhttp-hw.qtfm.cn":"lhttp.qtfm.cn",static_cast<unsigned>(s.qt));
}
// Bumped by the setup page so cached weather and media lists follow a new city at once.
inline std::atomic<uint32_t> mediaPrefsRevision{0};

#ifdef ESP_PLATFORM
template<class T> bool mediaLoad(const char* key,T& value){
    nvs_handle_t h;if(nvs_open("media",NVS_READONLY,&h)!=ESP_OK)return false;
    size_t size=sizeof(T);const bool ok=nvs_get_blob(h,key,&value,&size)==ESP_OK&&size==sizeof(T)&&value.version==T::kVersion;
    nvs_close(h);return ok;
}
template<class T> bool mediaSave(const char* key,const T& value){
    nvs_handle_t h;if(nvs_open("media",NVS_READWRITE,&h)!=ESP_OK)return false;
    const bool ok=nvs_set_blob(h,key,&value,sizeof(T))==ESP_OK&&nvs_commit(h)==ESP_OK;
    nvs_close(h);return ok;
}
#endif
}
