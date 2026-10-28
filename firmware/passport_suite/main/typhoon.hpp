#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace passport {
// 台风 (中央气象台, typhoon.nmc.cn). Grades as CMA names them, weakest first; kTyphoonGrades[6] is unknown.
inline constexpr const char* kTyphoonCodes[]{"TD","TS","STS","TY","STY","SuperTY"};
inline constexpr const char* kTyphoonGrades[]{"热带低压","热带风暴","强热带风暴","台风","强台风","超强台风","未知"};
inline constexpr uint32_t kTyphoonColors[]{0x3ccf6e,0x3d8bff,0xf5d33d,0xff9a3c,0xf06ce0,0xf04848,0x9bbbd1};
inline uint8_t typhoonGrade(const char* code){for(uint8_t i=0;i<6;++i)if(!strcmp(code,kTyphoonCodes[i]))return i;return 6;}
// Beaufort force of a sustained wind in m/s (China's GB/T 28591 adds 13-17 above hurricane force); 18: above 17.
inline unsigned beaufort(unsigned ms){
    static constexpr float top[]{0.2f,1.5f,3.3f,5.4f,7.9f,10.7f,13.8f,17.1f,20.7f,24.4f,28.4f,32.6f,36.9f,41.4f,46.1f,50.9f,56.0f,61.2f};
    unsigned force=0;while(force<std::size(top)&&ms>top[force])++force;return force;
}
// "WNW" -> "西偏北"; "0" (no motion given) -> "".
inline const char* typhoonHeading(const char* compass){
    static constexpr const char* codes[]{"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};
    static constexpr const char* names[]{"北","北偏东","东北","东偏北","东","东偏南","东南","南偏东","南","南偏西","西南","西偏南","西","西偏北","西北","北偏西"};
    for(unsigned i=0;i<16;++i)if(!strcmp(compass,codes[i]))return names[i];
    return "";
}
// Great-circle distance in km.
inline double distanceKm(double lat1,double lon1,double lat2,double lon2){
    constexpr double rad=3.14159265358979/180;
    const double a=std::sin((lat2-lat1)*rad/2),b=std::sin((lon2-lon1)*rad/2);
    return 2*6371*std::asin(std::min(1.0,std::sqrt(a*a+std::cos(lat1*rad)*std::cos(lat2*rad)*b*b)));
}
// Positions in tenths of a degree east and north.
struct TyphoonPoint{int16_t lon,lat;uint8_t grade;};
struct TyphoonAhead{int16_t lon,lat;uint8_t grade,hours;};
// One typhoon season's list (newest first) and the track of row `shown` (255: none). The track keeps at most
// kTrack points spread evenly over its life, the last always. at is the last fix's epoch; speed km/h; heading as sent.
// city: the setup page's city, at cityLon/cityLat (cityLat INT16_MIN: none chosen).
struct TyphoonView{
    static constexpr unsigned kRows=10,kTrack=40,kAhead=10;
    struct Row{char number[6];char name[16];bool active;} rows[kRows];uint8_t count;
    uint8_t shown,points,ahead;TyphoonPoint track[kTrack];TyphoonAhead forecast[kAhead];
    uint32_t at;uint16_t pressure,lowest;uint8_t wind,speed,peakWind,peakGrade;char heading[4];
    char city[32];int16_t cityLon,cityLat;
};
// Fixes in arrival order, at most kTrack: once full every other one is dropped and the spacing doubles. The
// newest fix replaces a skipped tail, so the track always ends where the typhoon is.
struct TyphoonTrack{
    TyphoonPoint* v;unsigned count{},stride{1},seen{};bool tail{};
    explicit TyphoonTrack(TyphoonPoint* out):v(out){}
    void add(TyphoonPoint p){
        if(tail){--count;tail=false;}
        if(count==TyphoonView::kTrack){for(unsigned i=0;i<count/2;++i)v[i]=v[2*i];count/=2;stride*=2;}
        tail=seen++%stride!=0;v[count++]=p;
    }
};
// The map: longitude scaled by the cosine of the middle latitude, fitted to w x h pixels around a box of
// positions (tenths of a degree), padded and at least kSpan degrees across.
struct TyphoonMap{
    static constexpr double kSpan=12,kPad=2;
    double west{},north{},scale{1},squeeze{1};int width{},height{};
    void fit(double w0,double e0,double s0,double n0,int w,int h){
        width=w;height=h;w0-=kPad;e0+=kPad;s0-=kPad;n0+=kPad;
        const double mid=(s0+n0)/2;squeeze=std::cos(mid*3.14159265358979/180);
        double lon=std::max(e0-w0,kSpan),lat=std::max(n0-s0,kSpan*h/w);
        scale=std::min(w/(lon*squeeze),h/lat);
        lon=w/(scale*squeeze);lat=h/scale;
        west=(w0+e0)/2-lon/2;north=(s0+n0)/2+lat/2;
    }
    int x(double lon)const{return int(std::lround((lon-west)*scale*squeeze));}
    int y(double lat)const{return int(std::lround((north-lat)*scale));}
    double lat(double y)const{return north-y/scale;}
};
}
