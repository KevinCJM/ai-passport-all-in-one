#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iterator>
#include "../../passport_suite/main/radio_stations.hpp"
#include "../../passport_suite/main/city_location.hpp"
#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace passport {
// City chosen on the setup page, by voice or by IP, for weather and local stations.
// topics and keyword are unused; they keep the layout of preferences saved by older firmware.
struct MediaPrefs{static constexpr uint32_t kVersion=2;uint32_t version{kVersion};char city[48]{};float latitude{},longitude{};char country[4]{};uint32_t topics{};char keyword[48]{};char province[24]{};};
// Keep the persisted v2 layout unchanged; validate every entry path with the same bounds.
inline bool setMediaCity(MediaPrefs& prefs,const char* city,const char* country,const char* province,double lat,double lon){
    if(!validCityText(city,sizeof(prefs.city))||!validCityCountry(country)||!validCityText(province,sizeof(prefs.province),true)||!validCityCoordinates(lat,lon))return false;
    strcpy(prefs.city,city);strcpy(prefs.country,country);strcpy(prefs.province,province);prefs.latitude=lat;prefs.longitude=lon;return true;
}

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
// 选台: a region chosen on the radio page (kRadioRegions index + 1; 0 = the city's own), kept apart from the city.
struct RadioPrefs{static constexpr uint32_t kVersion=1;uint32_t version{kVersion};uint8_t region{};};
// That region's own stations, most listened first (全国 = the national and online ones).
inline RadioPicks pickRegion(unsigned region){
    RadioPicks picks;picks.region=static_cast<int>(region);
    for(unsigned i=0;i<std::size(kRadioStations)&&picks.count<kMaxRadios;++i)if(kRadioStations[i].region==region)picks.index[picks.count++]=static_cast<uint16_t>(i);
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
// The clock follows the city: its UTC offset from Open-Meteo (timezone=auto), China fixed at UTC+8, saved so the
// clock is right from boot. ponytail: one fixed offset, a DST change shows at the next weather refresh (10 min).
struct CityZone{static constexpr uint32_t kVersion=1;uint32_t version{kVersion};int32_t offset{8*3600};};
inline void applyCityZone(int32_t offset){
    const int32_t minutes=(offset<0?-offset:offset)/60;char tz[16];  // POSIX: "LOC-8:00" is UTC+8
    snprintf(tz,sizeof(tz),"LOC%c%d:%02d",offset>0?'-':'+',int(minutes/60),int(minutes%60));setenv("TZ",tz,1);tzset();
}
}
