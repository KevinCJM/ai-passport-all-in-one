#pragma once
#include "macro.hpp"

namespace passport {
// A fixed 60-observation window, sorted by observation date, irrespective of wire order.
// No response body or JSON DOM is retained (the oil archive alone exceeds 300 KiB).
struct MacroPoints {
    unsigned count{};uint32_t dates[60]{};float values[60]{};
    bool add(uint32_t date,float value){
        if(!date||!std::isfinite(value))return true;
        unsigned at=0;while(at<count&&dates[at]<date)++at;
        if(at<count&&dates[at]==date)return values[at]==value;
        if(count==60){if(!at)return true;std::move(dates+1,dates+60,dates);std::move(values+1,values+60,values);--count;--at;}
        std::move_backward(dates+at,dates+count,dates+count+1);std::move_backward(values+at,values+count,values+count+1);
        dates[at]=date;values[at]=value;++count;return true;
    }
    bool merge(const MacroPoints& other){for(unsigned i=0;i<other.count;++i)if(!add(other.dates[i],other.values[i]))return false;return true;}
    MacroRow row(uint8_t series)const{return {series,count?values[count-1]:NAN,count>1?values[count-2]:NAN,count?dates[count-1]:0};}
    void chart(MacroView& view,unsigned row)const{
        view.chartFor=row;view.points=count;if(!count)return;
        const auto bounds=std::minmax_element(values,values+count);view.low=*bounds.first;view.high=*bounds.second;
        view.from=dates[0];view.zoomFrom=dates[count>12?count-12:0];view.to=dates[count-1];
        for(unsigned i=0;i<count;++i)view.chart[i]=view.high>view.low?int16_t(std::lround((values[i]-view.low)/(view.high-view.low)*999)):500;
    }
};
inline uint32_t macroParseDate(const char* text){
    unsigned y,m,d;int n=0;if(sscanf(text,"%4u-%2u-%2u%n",&y,&m,&d,&n)!=3||n!=10||y<1900||y>2200||m<1||m>12)return 0;
    if(text[10]&&strcmp(text+10," 00:00:00"))return 0;
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    return d&&d<=days[m-1]+unsigned(m==2&&y%4==0&&(y%100!=0||y%400==0))?y*10000+m*100+d:0;
}
class MacroJson {
public:
    explicit MacroJson(uint8_t series):series_(series){}
    MacroPoints points;uint32_t oldest{};
    void feed(const char* bytes,size_t size){for(size_t i=0;i<size&&!bad_;++i)put(bytes[i]);}
    bool finish(){if(raw_){raw_=false;scalar(false);}return !bad_&&done_&&!depth_&&!string_&&!hex_&&ok_&&unit_&&points.count;}
private:
    struct Frame{char kind;uint8_t state{};unsigned index{};char key[80]{};} frames_[8]{};
    uint8_t series_;unsigned depth_{},used_{},hex_{},code_{};bool bad_{},done_{},string_{},escape_{},raw_{},key_{},ok_{},unit_{};
    char token_[160]{};uint32_t date_{};float value_{NAN};
    const MacroSeries& spec()const{return kMacroSeries[series_];}
    void add(char c){if(used_+1<sizeof(token_))token_[used_++]=c;else bad_=true;}
    void complete(){if(!depth_){done_=true;return;}auto& f=frames_[depth_-1];if(f.kind=='{')f.state=3;else{f.state=1;++f.index;}}
    bool expecting()const{if(!depth_)return !done_;const auto& f=frames_[depth_-1];return f.kind=='{'?f.state==2:f.state==0||f.state==2;}
    bool path(const char* a,const char* b)const{return depth_>=3&&!strcmp(frames_[0].key,a)&&!strcmp(frames_[1].key,b);}
    void observe(){if(date_){if(!oldest||date_<oldest)oldest=date_;if(!points.add(date_,value_*spec().scale))bad_=true;}date_=0;value_=NAN;}
    static float number(const char* s){char* end;const float n=strtof(s,&end);return end!=s&&!*end&&std::isfinite(n)?n:NAN;}
    void field(bool quoted){
        if(!depth_)return;
        const auto& f=frames_[depth_-1];
        if(spec().source==MacroSource::Eastmoney){
            if(depth_==1&&!strcmp(f.key,"success")&&!quoted&&!strcmp(token_,"true"))ok_=unit_=true;
            if(depth_==4&&f.kind=='{'&&path("result","data")){
                if(!strcmp(f.key,series_==19?"TRADE_DATE":"REPORT_DATE")){date_=macroParseDate(token_);if(!date_)bad_=true;}
                if(!strcmp(f.key,spec().field)){value_=number(token_);if(!std::isfinite(value_)&&strcmp(token_,"null")&&*token_&&strcmp(token_,"--"))bad_=true;}
            }
        }else if(spec().source==MacroSource::Etf){
            if(depth_==1&&!strcmp(f.key,"status")&&!quoted&&!strcmp(token_,"200"))ok_=true;
            if(depth_==4&&path("data","keys")&&frames_[2].index==1&&!strcmp(f.key,"unit")&&!strcmp(token_,"吨"))unit_=true;
            if(depth_==4&&f.kind=='['&&path("data","values")){
                if(f.index==0){date_=macroParseDate(token_);if(!date_)bad_=true;}
                if(f.index==1){value_=number(token_);if(!std::isfinite(value_)&&strcmp(token_,"null"))bad_=true;}
            }
        }else if(spec().source==MacroSource::Oil){
            ok_=true;
            if(depth_==3&&!strcmp(frames_[0].key,"keys")&&frames_[1].index==0&&!strcmp(f.key,"unit")&&!strcmp(token_,"万桶/天"))unit_=true;
            if(depth_==4&&!strcmp(frames_[0].key,"values")&&!strcmp(frames_[2].key,"美国国内原油总量")&&f.kind=='['&&f.index==0){
                date_=macroParseDate(frames_[1].key);if(!date_)bad_=true;
                value_=number(token_);if(!std::isfinite(value_)&&strcmp(token_,"null"))bad_=true;observe();
            }
        }
    }
    void scalar(bool quoted){
        token_[used_]=0;
        if(!quoted){
            // JSON numbers: strtof alone also accepts +1, .1, NaN and hex, which JSON does not.
            const char* p=token_;if(*p=='-')++p;
            bool numeric=false;
            if(*p=='0'){++p;numeric=true;}else if(*p>='1'&&*p<='9'){numeric=true;while(isdigit(static_cast<unsigned char>(*p)))++p;}
            if(numeric&&*p=='.'){++p;if(!isdigit(static_cast<unsigned char>(*p)))numeric=false;while(isdigit(static_cast<unsigned char>(*p)))++p;}
            if(numeric&&(*p=='e'||*p=='E')){++p;if(*p=='+'||*p=='-')++p;if(!isdigit(static_cast<unsigned char>(*p)))numeric=false;while(isdigit(static_cast<unsigned char>(*p)))++p;}
            if((!numeric||*p)&&strcmp(token_,"null")&&strcmp(token_,"true")&&strcmp(token_,"false")){bad_=true;return;}
        }
        field(quoted);complete();used_=0;
    }
    void put(char c){
        if(hex_){unsigned h=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
            if(h==99){bad_=true;return;}code_=code_*16+h;if(!--hex_){if(code_>=0xd800&&code_<0xe000){bad_=true;return;}
                if(code_<0x80){if(!code_){bad_=true;return;}add(code_);}else if(code_<0x800){add(0xc0|(code_>>6));add(0x80|(code_&63));}else{add(0xe0|(code_>>12));add(0x80|((code_>>6)&63));add(0x80|(code_&63));}}return;}
        if(string_){
            if(escape_){escape_=false;if(c=='u'){hex_=4;code_=0;}else if(c=='"'||c=='\\'||c=='/')add(c);else if(strchr("bfnrt",c))add(' ');else bad_=true;return;}
            if(c=='\\'){escape_=true;return;}if(c=='"'){string_=false;token_[used_]=0;
                if(key_){auto& f=frames_[depth_-1];if(used_>=sizeof(f.key)){bad_=true;return;}strcpy(f.key,token_);f.state=1;used_=0;}else scalar(true);
            }else if(static_cast<unsigned char>(c)<32)bad_=true;else add(c);return;
        }
        if(raw_){if(c!=','&&c!='}'&&c!=']'&&!isspace(static_cast<unsigned char>(c))){add(c);return;}raw_=false;scalar(false);}
        if(isspace(static_cast<unsigned char>(c)))return;
        if(c=='}'||c==']'){
            if(!depth_){bad_=true;return;}const auto& f=frames_[depth_-1];
            if((c=='}'&&(f.kind!='{'||(f.state!=0&&f.state!=3)))||(c==']'&&(f.kind!='['||(f.state!=0&&f.state!=1)))){bad_=true;return;}
            if(depth_==4&&((spec().source==MacroSource::Eastmoney&&path("result","data"))||(spec().source==MacroSource::Etf&&path("data","values"))))observe();
            --depth_;complete();return;
        }
        if(depth_){auto& f=frames_[depth_-1];
            if(c==':'&&f.kind=='{'&&f.state==1){f.state=2;return;}
            if(c==','&&((f.kind=='{'&&f.state==3)||(f.kind=='['&&f.state==1))){f.state=f.kind=='{'?4:2;return;}
            if(f.kind=='{'&&(f.state==0||f.state==4)&&c=='"'){string_=key_=true;used_=0;return;}
        }
        if(!expecting()){bad_=true;return;}
        if(c=='{'||c=='['){if(depth_==std::size(frames_)){bad_=true;return;}frames_[depth_++]={c};return;}
        used_=0;key_=false;if(c=='"')string_=true;else{raw_=true;add(c);}
    }
};
}
