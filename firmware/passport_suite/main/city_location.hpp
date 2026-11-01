#pragma once
#include "city_list.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace passport {
struct CityCountry {const char* code;const char* name;const char* aliases;};
inline constexpr CityCountry kCityCountries[]{
    {"CN","中国","China|中国大陆"},{"HK","香港","Hong Kong|中国香港"},{"MO","澳门","Macau|Macao|中国澳门"},{"TW","台湾","Taiwan|中国台湾"},
#include "city_countries.inc"
};
inline bool validCityCountry(const char* code){return code&&code[0]>='A'&&code[0]<='Z'&&code[1]>='A'&&code[1]<='Z'&&!code[2];}
inline bool validCityCoordinates(double lat,double lon){return std::isfinite(lat)&&std::isfinite(lon)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180;}
inline bool validCityText(const char* text,size_t capacity,bool empty=false){
    if(!text||(!empty&&!*text)||strlen(text)>=capacity)return false;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(text);*p;++p)if(*p<32||*p==127)return false;
    return true;
}
inline const char* cityCountryName(const char* code){for(const auto& c:kCityCountries)if(!strcmp(c.code,code))return c.name;return "";}
inline char cityLower(char c){return c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
inline bool cityWord(char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');}
// ASCII names match without case; require word boundaries so e.g. Nice does not match "Venice".
inline bool cityMatch(const char* text,const char* name,size_t n,bool exact=false){
    if(!n)return false;
    for(size_t i=0;text[i];++i){
        size_t j=0;while(j<n&&text[i+j]&&cityLower(text[i+j])==cityLower(name[j]))++j;
        if(j!=n)continue;
        if(exact)return !i&&(!text[n]||!strcmp(text+n,"市"));
        if((!cityWord(name[0])||!i||!cityWord(text[i-1]))&&(!cityWord(name[n-1])||!cityWord(text[i+n])))return true;
    }
    return false;
}
inline size_t cityAliasMatch(const char* text,const char* aliases,bool exact=false){
    size_t best=0;
    for(const char* start=aliases;*start;){const char* end=strchr(start,'|');const size_t n=end?size_t(end-start):strlen(start);if(n>best&&cityMatch(text,start,n,exact))best=n;if(!end)break;start=end+1;}
    return best;
}
// Always filter IP matches by country: London, CA must not select London, GB.
inline const CityEntry* cityNamed(const char* name,const char* country=nullptr){
    if(!name)return nullptr;
    for(const auto& c:kCities)if((!country||!strcmp(country,c.country))&&(cityMatch(name,c.name,strlen(c.name),true)||cityAliasMatch(name,c.aliases,true)))return &c;
    return nullptr;
}
// Country-qualified phrases disambiguate duplicates. An unqualified duplicate is refused.
inline const CityEntry* spokenCity(const char* heard){
    if(!heard)return nullptr;
    const char* country=nullptr;size_t countryLength=0;
    for(const auto& c:kCityCountries){const size_t n=cityMatch(heard,c.name,strlen(c.name))?strlen(c.name):0;const size_t score=std::max(n,cityAliasMatch(heard,c.aliases));if(score>countryLength){country=c.code;countryLength=score;}}
    const CityEntry* found=nullptr;size_t length=0;bool ambiguous=false;
    for(const auto& c:kCities){
        if(country&&strcmp(country,c.country))continue;
        const size_t score=std::max(cityMatch(heard,c.name,strlen(c.name))?strlen(c.name):size_t{0},cityAliasMatch(heard,c.aliases));
        if(score>length){found=&c;length=score;ambiguous=false;}
        else if(score&&score==length&&found&&strcmp(found->country,c.country))ambiguous=true;
    }
    // London defaults to the familiar UK city; Canada is selected explicitly with its country.
    if(ambiguous&&found&&!strcmp(found->name,"伦敦")&&!country)return cityNamed("伦敦","GB");
    return ambiguous?nullptr:found;
}
}
