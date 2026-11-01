"""Exercise real city entry paths, offline catalog and timezone boundaries."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CityTests(unittest.TestCase):
    def test_catalog_reproducible_and_bounded(self):
        subprocess.run(['python3', str(ROOT / 'scripts/generate_passport_cities.py'), '--check'], check=True)
        countries = json.loads((ROOT / 'data/international-cities.json').read_text())['countries']
        self.assertEqual(len(countries), 51)
        self.assertEqual(sum(len(c['cities']) for c in countries), 155)
        seen = set()
        for country in countries:
            self.assertLess(len(country['name'].encode()), 24)
            for city in country['cities']:
                identity = country['code'], city['name']
                self.assertNotIn(identity, seen)
                seen.add(identity)
                self.assertLess(len(city['name'].encode()), 48)
                self.assertTrue(-90 <= city['latitude'] <= 90 and -180 <= city['longitude'] <= 180)
                self.assertTrue(city['geoname_id'] > 0 and city['aliases'])

    def test_native_voice_ip_validation_and_dates(self):
        # Compile the actual production methods, stubbing only HTTP and persistence.
        native = (ROOT / 'firmware/passport_device/main/native_services.cpp').read_text()
        methods = native[native.index('namespace {\n// The city as Settings'):native.index('void NativeServices::closeSocket')]
        status_method = native[native.index('void NativeServices::status('):native.index('bool NativeServices::request(')]
        portal = (ROOT / 'firmware/passport_device/main/wifi_portal.cpp').read_text()
        portal_method = portal[portal.index('esp_err_t WifiPortal::mediaSave'):portal.index('esp_err_t WifiPortal::ratePrefs')]
        source = r'''
#include "voice_add.hpp"
#include "weather.hpp"
#include "media_catalog.hpp"
#include "cJSON.h"
#include <cassert>
#include <limits>
#include <string>
namespace passport {
inline MediaPrefs persisted;
inline unsigned writes=0;
inline bool saveFails=false;
template<class T> bool mediaLoad(const char*,T& v){v=persisted;return true;}
template<class T> bool mediaSave(const char*,const T& v){if(saveFails)return false;persisted=v;++writes;return true;}
template<size_t N> void copy(char (&out)[N],const char* value){snprintf(out,N,"%s",value);}
struct Json{cJSON* value;~Json(){cJSON_Delete(value);}};
const char* str(const cJSON* o,const char* key){const char* v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,key));return v?v:"";}
double num(const cJSON* o,const char* key){const cJSON* v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsNumber(v)?v->valuedouble:NAN;}
class NativeServices {
public:
    char notice_[160]{},city_[80]{},stt_[8]{},answer_[8]{};std::string response,phase,state,value;bool network=true;
    bool failed_=false,listening_=false,asr_=false,transcript=false;int current_=0;
    void setCity(const char*);void locate();void cityName(char*,size_t)const;
    void status(const char*,const char* p="ready");
    bool transcribing()const{return transcript;}
    void publish(int,const char* text,const char* p,const char* v,const char* const* =nullptr){state=text;phase=p;value=v;}
    bool fetch(const char* url,char* out,size_t size){assert(strstr(url,"ip-api.com/json/")&&strstr(url,"lang=zh-CN"));if(!network)return false;assert(response.size()<size);memcpy(out,response.c_str(),response.size()+1);return true;}
};
@METHODS@
@STATUS@
using esp_err_t=int;
constexpr int ESP_FAIL=-1;
struct httpd_req_t {std::string body;int content_len=0,status=0,read=0;bool allowed=true,origin=true;};
int httpd_req_recv(httpd_req_t* r,char* out,size_t size){const size_t n=std::min(size,r->body.size()-r->read);memcpy(out,r->body.data()+r->read,n);r->read+=n;return int(n);}
int reply(httpd_req_t* r,const char* code,const char*){r->status=atoi(code);return 0;}
class WifiPortal {
public:
    bool access(httpd_req_t* r){return r->allowed;}bool sameOrigin(httpd_req_t* r){return r->origin;}
    esp_err_t mediaSave(httpd_req_t*);
};
@PORTAL@
}
using namespace passport;
int main(){
    const std::pair<const char*,const char*> samples[]{
        {"我在美国纽约","US"},{"美国洛杉矶","US"},{"加拿大温哥华","CA"},{"日本东京","JP"},
        {"韩国首尔","KR"},{"澳洲悉尼","AU"},{"法国巴黎","FR"},{"德国慕尼黑","DE"},
        {"英国伦敦","GB"},{"加拿大伦敦","CA"},{"我在 London","GB"},{"I'm in Venice","IT"},
        {"Vancouver, Canada","CA"},{"我在苏州","CN"},{"呼和浩特","CN"},{"香港","HK"},
        {"三藩市","US"},{"雪梨","AU"},{"汉城","KR"},{"New York","US"},{"TOKYO","JP"},
        {"中国香港","HK"},{"中国澳门","MO"},{"中国台湾台北","TW"}};
    NativeServices service;
    // A timeout must finish capture before publishing the error, otherwise Settings still blocks Up/Down.
    service.transcript=true;service.listening_=true;service.status("正在听");assert(service.value=="rec");
    service.listening_=false;service.asr_=true;service.status("正在识别");assert(service.value=="asr");
    service.status("识别超时，请重试","error");assert(service.failed_&&!service.listening_&&!service.asr_&&service.value.empty());
    service.listening_=true;service.status("录音失败","error");assert(!service.listening_&&service.value.empty());
    service.transcript=false;
    for(const auto& [phrase,code]:samples){auto* c=spokenCity(phrase);assert(c&&!strcmp(c->country,code));unsigned before=writes;service.setCity(phrase);assert(writes==before+1&&!strcmp(persisted.country,code)&&persisted.latitude==c->latitude);}
    assert(!spokenCity("未收录的城市"));assert(!spokenCity("我在美国温哥华"));
    assert(!cityNamed("London","US"));assert(!strcmp(cityNamed("London","CA")->country,"CA"));
    assert(!strcmp(cityNamed("杭州市","CN")->name,"杭州"));
    assert(!strcmp(spokenCity("I'm in Venice")->country,"IT"));
    for(const auto& c:kCities){
        assert(validCityCoordinates(c.latitude,c.longitude)&&validCityCountry(c.country));assert(validCityText(c.name,48)&&validCityText(c.province,24,true));
        std::string phrase=std::string(cityCountryName(c.country))+c.name;
        const auto* found=spokenCity(phrase.c_str());assert(found&&!strcmp(found->name,c.name)&&!strcmp(found->country,c.country));
    }
    const auto ip=[&](const char* code,const char* name,double lat,double lon){
        char body[700];snprintf(body,sizeof(body),R"({"status":"success","countryCode":"%s","country":"海外","city":"%s","regionName":"州","lat":%.6f,"lon":%.6f})",code,name,lat,lon);
        service.response=body;unsigned before=writes;service.locate();assert(writes==before+1&&!strcmp(persisted.country,code));assert(std::fabs(persisted.latitude-lat)<0.0001&&std::fabs(persisted.longitude-lon)<0.0001);
    };
    ip("US","New York",40.71,-74.01);assert(!strcmp(persisted.city,"纽约"));
    ip("CA","London",42.98,-81.25);assert(!strcmp(persisted.city,"伦敦"));
    ip("GB","London",51.50,-0.12);ip("US","Portland",43.66,-70.26); // Preserve Maine's coordinates.
    ip("JP","Tokyo",35.68,139.69);ip("KR","Seoul",37.56,126.97);ip("AU","Sydney",-33.86,151.20);
    ip("FR","Paris",48.85,2.35);ip("DE","Berlin",52.52,13.40);ip("CA","Kelowna",49.88,-119.49);
    ip("CN","杭州市",30.27,120.16);assert(!strcmp(persisted.city,"杭州"));
    ip("HK","Hong Kong",22.32,114.17);assert(!*persisted.province);
    ip("NZ","Wellington",-41.28,174.77); // No full-world table is needed for IP fallback.
    for(const char* body:{"{}","not JSON",R"({"status":"fail"})",R"({"status":"success","countryCode":"US","city":"Boston","lat":91,"lon":-71})",
         R"({"status":"success","countryCode":"US","city":"Boston","lon":-71})",R"({"status":"success","countryCode":"USA","city":"Boston","lat":42,"lon":-71})",
         R"({"status":"success","countryCode":"US","city":"","lat":42,"lon":-71})"}){
        const auto old=persisted;unsigned before=writes;service.response=body;service.locate();assert(writes==before&&!memcmp(&old,&persisted,sizeof(old)));
    }
    service.network=false;unsigned before=writes;service.locate();assert(writes==before&&service.phase=="error");
    service.network=true;saveFails=true;service.response=R"({"status":"success","countryCode":"US","city":"Boston","lat":42,"lon":-71})";service.locate();assert(writes==before&&service.phase=="error");saveFails=false;
    MediaPrefs prefs;const auto unchanged=prefs;
    for(const char* code:{"","U","us","USA","U1"})assert(!setMediaCity(prefs,"test",code,"",0,0));
    assert(!setMediaCity(prefs,"test","US","",NAN,0));assert(!setMediaCity(prefs,"test","US","",0,181));
    assert(!setMediaCity(prefs,"test\n","US","",0,0));assert(!memcmp(&prefs,&unchanged,sizeof(prefs)));
    const std::string longName(48,'x');assert(!setMediaCity(prefs,longName.c_str(),"US","",0,0));
    assert(forecastWeekday("2026-11-01")==0&&forecastWeekday("2026-10-31")==6);
    assert(forecastWeekday("2024-02-29")==4&&forecastWeekday("2000-02-29")==2);
    for(const char* invalid:{"","2026-02-29","2100-02-29","2026-00-01","2026-13-01","2026-11-31","2026-0x-01"})assert(forecastWeekday(invalid)==255);
    assert(forecastWeekday(nullptr)==255);
    WifiPortal portal;
    for(const char* cc:{"CN","HK","US","CA","JP","KR","AU","GB","FR","DE"}){
        httpd_req_t r;r.body=std::string(R"({"city":"test","province":"","country":")")+cc+R"(","latitude":-33.86,"longitude":151.2})";r.content_len=r.body.size();
        unsigned previous=writes;assert(portal.mediaSave(&r)==0&&r.status==200&&writes==previous+1&&!strcmp(persisted.country,cc));
    }
    for(const char* bad:{"not json",R"({"city":"test","province":"","country":"US","latitude":null,"longitude":0})",
         R"({"city":"test","province":"","country":"USA","latitude":0,"longitude":0})",
         R"({"city":"test","province":"","country":"US","latitude":0,"longitude":181})"}){
        httpd_req_t r;r.body=bad;r.content_len=r.body.size();unsigned previous=writes;portal.mediaSave(&r);assert(r.status==400&&writes==previous);
    }
    for(unsigned kind=0;kind<2;++kind){httpd_req_t r;r.allowed=kind==0;r.origin=kind==1;unsigned previous=writes;portal.mediaSave(&r);assert(r.status==403&&writes==previous);}
    setenv("TZ","CST-8",1);tzset();assert(forecastWeekday("2026-10-31")==6);
    setenv("TZ","EST5EDT",1);tzset();assert(forecastWeekday("2026-11-01")==0);
    static_assert(sizeof(MediaPrefs)==140); // Existing NVS preferences remain readable.
}
'''.replace('@METHODS@', methods).replace('@STATUS@', status_method).replace('@PORTAL@', portal_method)
        compiler = shutil.which('clang++') or shutil.which('g++')
        ccompiler = shutil.which('clang') or shutil.which('gcc')
        with tempfile.TemporaryDirectory(prefix='passport-cities-') as directory:
            path = Path(directory)
            (path / 'cities.cpp').write_text(source)
            cjson = ROOT / 'deps/espressif__cjson/cJSON'
            flags = ['-fsanitize=address,undefined', '-g']
            subprocess.run([ccompiler, '-std=c11', *flags, '-I', str(cjson), '-c', str(cjson / 'cJSON.c'), '-o', str(path / 'json.o')], check=True)
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                            '-I', str(ROOT / 'firmware/passport_suite/main'), '-I', str(ROOT / 'firmware/passport_device/main'),
                            '-I', str(cjson), str(path / 'cities.cpp'), str(path / 'json.o'), '-o', str(path / 'cities')], check=True)
            subprocess.run([str(path / 'cities')], check=True)


if __name__ == '__main__':
    unittest.main()
