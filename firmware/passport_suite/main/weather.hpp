#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "simplify_map.hpp"

namespace passport {
// The UI font is GB2312; services may still answer in traditional script (ipwho.is names Vancouver so).
// Keep traditional characters out of firmware sources: the font generator would embed them.
// Rewrites mapped 3-byte UTF-8 characters in place to their drawable simplified form.
inline void simplifyText(char* text){
    for(auto* p=reinterpret_cast<unsigned char*>(text);*p;++p){
        if((p[0]&0xf0)!=0xe0||(p[1]&0xc0)!=0x80||(p[2]&0xc0)!=0x80)continue;
        const uint16_t code=(p[0]&0x0f)<<12|(p[1]&0x3f)<<6|(p[2]&0x3f);
        const auto* at=std::lower_bound(std::begin(kTraditional),std::end(kTraditional),code);
        if(at!=std::end(kTraditional)&&*at==code){const uint16_t s=kSimplified[at-kTraditional];p[0]=0xe0|s>>12;p[1]=0x80|(s>>6&0x3f);p[2]=0x80|(s&0x3f);}
        p+=2;
    }
}
// Copies at most n bytes of in, cut back to a whole UTF-8 character so it fits.
inline void fitText(char* out,size_t capacity,const char* in,size_t n){if(n>=capacity)n=capacity-1;while(n&&(static_cast<unsigned char>(in[n])&0xc0)==0x80)--n;std::memcpy(out,in,n);out[n]=0;}
enum class WeatherIcon { Unknown, Sun, PartlyCloudy, Cloud, Fog, Rain, Snow, Thunder };
constexpr WeatherIcon weatherIcon(int code){
    if(code==0)return WeatherIcon::Sun;
    if(code==1||code==2)return WeatherIcon::PartlyCloudy;
    if(code==3)return WeatherIcon::Cloud;
    if(code==45||code==48)return WeatherIcon::Fog;
    if(code>=95&&code<=99)return WeatherIcon::Thunder;
    if((code>=71&&code<=77)||code==85||code==86)return WeatherIcon::Snow;
    if((code>=51&&code<=67)||(code>=80&&code<=82))return WeatherIcon::Rain;
    return WeatherIcon::Unknown;
}
constexpr const char* weatherName(int code){
    switch(code){case 0:return "晴";case 1:return "大部晴朗";case 2:return "多云";case 3:return "阴";case 45:case 48:return "雾";
    case 51:return "小毛毛雨";case 53:return "毛毛雨";case 55:return "浓毛毛雨";case 56:case 57:case 66:case 67:return "冻雨";case 61:return "小雨";case 63:return "中雨";case 65:return "大雨";
    case 71:return "小雪";case 73:return "中雪";case 75:return "大雪";case 77:return "米雪";case 80:case 81:case 82:return "阵雨";case 85:case 86:return "阵雪";case 95:case 96:case 97:case 99:return "雷雨";default:return "天气未知";}
}
// 空气质量 by China's HJ 633-2012, as Chinese weather apps show it (Open-Meteo itself only offers the US and EU
// scales, which read higher). Readings in µg/m³ except CO in mg/m³: PM2.5 and PM10 on their 24-hour scales (pass
// 24-hour averages), O3, NO2, SO2 and CO hourly; SO2 past 800 continues on its 24-hour scale. The pollutant with
// the highest sub-index is the primary one; aqi -1 when nothing was read.
struct ChinaAqi{int aqi;unsigned primary;};
inline ChinaAqi chinaAqi(const float (&reading)[6]){
    static constexpr float scale[6][8]{{0,35,75,115,150,250,350,500},{0,50,150,250,350,420,500,600},{0,160,200,300,400,800,1000,1200},
                                       {0,100,200,700,1200,2340,3090,3840},{0,150,500,650,800,1600,2100,2620},{0,5,10,35,60,90,120,150}};
    static constexpr float index[8]{0,50,100,150,200,300,400,500};
    ChinaAqi worst{-1,0};
    for(unsigned p=0;p<6;++p){
        const float c=reading[p];if(!std::isfinite(c)||c<0)continue;
        unsigned k=1;while(k<7&&c>scale[p][k])++k;
        const float sub=index[k-1]+(index[k]-index[k-1])*(std::min(c,scale[p][7])-scale[p][k-1])/(scale[p][k]-scale[p][k-1]);
        const int v=static_cast<int>(std::ceil(sub-1e-3f));if(v>worst.aqi)worst={v,p};
    }
    return worst;
}
constexpr const char* aqiLevel(int aqi){return aqi<=50?"优":aqi<=100?"良":aqi<=150?"轻度污染":aqi<=200?"中度污染":aqi<=300?"重度污染":"严重污染";}
constexpr const char* aqiPollutant(unsigned p){constexpr const char* names[]{"PM2.5","PM10","臭氧","二氧化氮","二氧化硫","一氧化碳"};return names[p%6];}
// 气象预警 (中央气象台, nmc.cn): a province's signals in force, kept when issued for the city (its counties too) or
// by the province's own observatory; one entry per kind at its highest level, the newest of that level first seen
// (a city's own preferred to the province's). Level 0 blue, 1 yellow, 2 orange, 3 red; time is "MM/DD HH:MM";
// id names its page on nmc.cn. A provincial one may cover only part of the province: see warningCovers.
struct WeatherWarning{char type[16],where[32],time[12],id[32];uint8_t level,places;bool provincial;};
constexpr const char* kWarningLevels[]{"蓝色","黄色","橙色","红色"};
constexpr uint32_t kWarningColors[]{0x3d8bff,0xf5d33d,0xff9a3c,0xf04848};
// title "山西省太原市清徐县气象台发布大风蓝色预警信号", issued "2026/10/26 11:02"; lifted signals are skipped.
inline bool addWarning(WeatherWarning* list,uint8_t& count,unsigned capacity,const char* title,const char* issued,const char* id,const char* city,const char* province){
    const char* office=strstr(title,"气象台");if(!office)office=strstr(title,"气象局");if(!office||!*city)return false;
    const char* kind=nullptr;
    for(const char* verb:{"发布","更新","升级","降级","继续发布"})if(!strncmp(office+9,verb,strlen(verb))){kind=office+9+strlen(verb);break;}
    if(!kind)return false;
    const char* color=nullptr;unsigned level=0;
    for(unsigned l=0;l<4;++l)if(const char* at=strstr(kind,kWarningLevels[l]);at&&(!color||at<color)){color=at;level=l;}
    if(!color||color==kind||strncmp(color+6,"预警",6))return false;
    // Whose: past the province's name, the city ("吉林省吉林市", not every "吉林省..."), or nothing but the province.
    char issuer[64];snprintf(issuer,sizeof(issuer),"%.*s",int(office-title),title);
    const size_t p=strlen(province);const bool inProvince=p&&!strncmp(issuer,province,p);
    const char* local=strstr(issuer+(inProvince?p:0),city);
    bool provincial=inProvince&&!local;
    for(const char* part:{"市","县","州","盟","旗"})if(provincial&&strstr(issuer+p,part))provincial=false;
    if(!local&&!provincial)return false;
    WeatherWarning w{};fitText(w.type,sizeof(w.type),kind,color-kind);
    // The province's own: from its last mention ("浙江省自然资源厅、浙江省" -> "浙江省").
    const char* where=local;if(!where){where=issuer;for(const char* at=issuer;(at=strstr(at+1,province));)where=at;}
    fitText(w.where,sizeof(w.where),where,strlen(where));
    if(strlen(issued)>=16)snprintf(w.time,sizeof(w.time),"%.5s %.5s",issued+5,issued+11);
    fitText(w.id,sizeof(w.id),id,strlen(id));
    w.level=level;w.places=1;w.provincial=!local;
    // The reply lists the province's own once more ("provinceAlarms").
    for(unsigned i=0;i<count;++i)if(*id&&!strcmp(list[i].id,id))return true;
    for(unsigned i=0;i<count;++i)if(!strcmp(list[i].type,w.type)){
        if(level>list[i].level||(level==list[i].level&&list[i].provincial&&local)){w.places=list[i].places+1;list[i]=w;}else ++list[i].places;
        return true;
    }
    // Full: a higher signal replaces the lowest one.
    if(count<capacity){list[count++]=w;return true;}
    auto* low=std::min_element(list,list+count,[](const WeatherWarning& a,const WeatherWarning& b){return a.level<b.level;});
    if(low->level<level)*low=w;
    return true;
}
// A warning's text (nmc.cn page): "浙江省…联合更新发布地质灾害气象风险黄色预警：10月25日20时至…舟山市…".
// A provincial warning concerns the city when its text names it or the whole province.
inline bool warningCovers(const char* text,const char* city){return (*city&&strstr(text,city))||strstr(text,"全省");}
// What it says, past the heading that repeats who issued what and when.
inline const char* warningBody(const char* text){const char* colon=strstr(text,"：");return colon&&colon-text<200&&colon[3]?colon+3:text;}
// 天气预报 (Open-Meteo): the next seven days from today, then 24 hours from hour0 (local time).
// Temperatures are whole degrees; rain is the chance of precipitation in percent, 255 when unknown.
// Air: China AQI for airHours hours from hour0 (-1 unknown), now's primary pollutant, PM2.5 and PM10;
// today's sunrise and sunset in minutes after midnight (0xffff unknown) and its highest UV index in tenths (255).
// Warnings highest first; warnings 0xff when they could not be read. detail: warning detailFor's text (0xff none).
struct WeatherForecast {
    char city[64];uint8_t days,hours,hour0;
    struct Day{int8_t low,high;uint8_t code,rain;} day[7];
    struct Hour{int8_t temp;uint8_t code,rain;} hour[24];
    uint8_t airHours,primary;int16_t aqi[24];uint16_t pm25,pm10,sunrise,sunset;uint8_t uv;
    uint8_t warnings;WeatherWarning warning[4];
    uint8_t detailFor;char detail[512];
};
// warningLevel -1: none in force (or unknown).
struct WeatherReading {
    bool known{},airKnown{};
    char city[64]{},condition[32]{},warning[16]{};int8_t warningLevel{-1};
    double temperature{},minimum{},maximum{},humidity{},aqi{};
    int code{-1};
    bool update(const char* place,const char* description,double t,double low,double high,double rh,double wmo,double air){
        if(!*place||!*description||!std::isfinite(t)||!std::isfinite(low)||!std::isfinite(high)||
           !std::isfinite(rh)||!std::isfinite(wmo)||t< -150||t>100||low< -150||high>100||low>high||rh<0||rh>100||wmo<0||wmo>99||std::trunc(wmo)!=wmo)return false;
        fitText(city,sizeof(city),place,std::strlen(place));fitText(condition,sizeof(condition),description,std::strlen(description));simplifyText(city);simplifyText(condition);
        temperature=t;minimum=low;maximum=high;humidity=rh;code=static_cast<int>(wmo);
        airKnown=std::isfinite(air)&&air>=0&&air<=1000;aqi=airKnown?air:0;known=true;return true;
    }
};
}
