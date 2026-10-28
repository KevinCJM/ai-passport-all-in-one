#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace passport {
// 播客: Ximalaya's legacy mobile API needs no key and serves 64k MP3 over plain HTTP, which the radio's
// 8 KiB task can play (no room for TLS). The phone setup page edits the list; these are the defaults.
struct PodcastList{
    static constexpr uint32_t kVersion=1;static constexpr unsigned kMax=12;
    struct Item{uint32_t album;char name[40];};
    uint32_t version{kVersion};uint8_t count{};Item items[kMax]{};
};
inline PodcastList defaultPodcasts(){
    return {PodcastList::kVersion,4,{{51076156,"声动早咖啡"},{5574153,"日谈公园"},{35417311,"BBC 随身英语"},{82749641,"TED 演讲精选"}}};
}
// Where each program was left: its episode (oldest = 0), that episode's track id, and the byte reached in its MP3.
struct PodcastSpots{
    static constexpr uint32_t kVersion=1;
    struct Spot{uint32_t album,track,offset;uint16_t episode;};
    uint32_t version{kVersion};Spot spots[PodcastList::kMax]{};
};
// The program's spot; with create, a free one or one whose program has left the list (there is always one).
inline PodcastSpots::Spot* podcastSpot(PodcastSpots& s,const PodcastList& list,uint32_t album,bool create){
    for(auto& spot:s.spots)if(spot.album==album)return &spot;
    if(create)for(auto& spot:s.spots){
        bool listed=false;for(unsigned i=0;i<list.count;++i)listed|=list.items[i].album==spot.album;
        if(!spot.album||!listed){spot={album,0,0,0};return &spot;}
    }
    return nullptr;
}
// One episode per request: page N of one, oldest first, or the newest (whose reply gives the episode count).
inline void podcastTrackUrl(char* out,size_t size,uint32_t album,int episode){
    snprintf(out,size,"http://mobile.ximalaya.com/mobile/others/ca/album/track/%lu/%s/%d/1",static_cast<unsigned long>(album),episode<0?"true":"false",episode<0?1:episode+1);
}
// Picks "key":value pairs out of a JSON stream without holding it, so a 15 KB reply needs no 15 KB buffer.
// Each key in keys is reported at any depth once `after` has gone by: strings unescaped (cut at 191 bytes,
// on a character boundary), numbers and literals as written; object and array values are skipped.
class JsonScan{
public:
    using Field=void(*)(void* context,unsigned key,const char* value);
    JsonScan(const char* const* keys,unsigned count,const char* after,Field field,void* context)
        :keys_(keys),count_(count),after_(after),field_(field),context_(context),started_(!after){}
    void reset(){*this=JsonScan(keys_,count_,after_,field_,context_);}
    void feed(const char* data,size_t size){for(size_t i=0;i<size;++i)put(data[i]);}
private:
    const char* const* keys_;unsigned count_;const char* after_;Field field_;void* context_;
    bool started_,string_{},escape_{},key_{},raw_{},full_{};int value_=-1;unsigned matched_{},used_{},hex_{},code_{};
    char text_[192]{};
    void add(unsigned char c){if(used_<sizeof(text_)-1)text_[used_++]=c;else full_=true;}
    void emit(){
        // A cut value drops its last (possibly partial) character.
        if(full_){while(used_&&(text_[used_-1]&0xc0)==0x80)--used_;if(used_&&(text_[used_-1]&0x80))--used_;}
        text_[used_]=0;field_(context_,value_,text_);value_=-1;used_=0;full_=false;
    }
    static bool space(char c){return c==' '||c=='\n'||c=='\r'||c=='\t';}
    void put(char c){
        if(!started_){matched_=c==after_[matched_]?matched_+1:c==after_[0];started_=!after_[matched_];return;}
        if(hex_){
            code_=code_*16+(isdigit(static_cast<unsigned char>(c))?c-'0':(c|32)-'a'+10);
            if(!--hex_){const unsigned u=code_>=0xd800&&code_<0xe000?'?':code_;
                if(u<0x80)add(u);else if(u<0x800){add(0xc0|u>>6);add(0x80|(u&63));}else{add(0xe0|u>>12);add(0x80|(u>>6&63));add(0x80|(u&63));}}
            return;
        }
        if(string_){
            if(escape_){escape_=false;if(c=='u'){hex_=4;code_=0;}else add(c=='n'||c=='r'||c=='t'||c=='b'||c=='f'?' ':c);return;}
            if(c=='\\'){escape_=true;return;}
            if(c!='"'){add(c);return;}
            string_=false;
            if(value_>=0)emit();else{text_[used_]=0;key_=true;}
            return;
        }
        if(raw_){if(c!=','&&c!='}'&&c!=']'&&!space(c)){add(c);return;}raw_=false;emit();}
        if(space(c))return;
        if(key_){key_=false;if(c==':'){for(unsigned k=0;k<count_;++k)if(!strcmp(text_,keys_[k]))value_=k;used_=0;full_=false;return;}}
        if(value_>=0&&c!='"'){if(c=='{'||c=='[')value_=-1;else{raw_=true;add(c);}return;}
        if(c=='"'){string_=true;used_=0;full_=false;}
    }
};
}
