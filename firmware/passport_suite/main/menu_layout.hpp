#pragma once
#include "apps.hpp"
#include <algorithm>
#include <cstring>

namespace passport {
inline int menuCatalogIndex(App app) {
    const auto at=std::find(kFunctions.begin(),kFunctions.end(),app);
    return at==kFunctions.end()?-1:static_cast<int>(at-kFunctions.begin());
}
inline unsigned menuCategory(App app) {
    const int index=menuCatalogIndex(app);
    return index<0?0:menuPageOf(static_cast<unsigned>(index));
}
// All fields are bytes: a stable, endian-independent NVS/wire record. App IDs, never menu positions.
struct MenuLayout {
    static constexpr unsigned kVersion=1,kApps=64,kPages=8,kName=25,kBytes=481;
    enum Mode : uint8_t { Categories, Continuous, Custom };
    uint8_t version{kVersion},mode{},catalogCount{static_cast<uint8_t>(kFunctions.size())},pageCount{5};
    uint8_t hidden[8]{},categories[5]{0,1,2,3,4},order[3][kApps]{},assignment[kApps]{},pages[kPages]{};
    char names[kPages][kName]{};
    bool shown(App app)const {const auto id=static_cast<unsigned>(app);return id<kApps&&!(hidden[id/8]&(1u<<(id%8)));}
    void show(App app,bool yes){const auto id=static_cast<unsigned>(app);if(id<kApps){if(yes)hidden[id/8]&=~(1u<<(id%8));else hidden[id/8]|=1u<<(id%8);}}
    static bool textValid(const char* text) {
        unsigned chars=0;
        for(unsigned i=0;i<kName;) {
            const auto c=static_cast<unsigned char>(text[i]);if(!c)return chars>0;
            if(c<32||c==127||++chars>8)return false;
            const unsigned n=c<128?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
            if(!n||i+n>=kName)return false;
            for(unsigned j=1;j<n;++j)if((static_cast<unsigned char>(text[i+j])&0xc0)!=0x80)return false;
            if(n>=3){const auto d=static_cast<unsigned char>(text[i+1]);if((c==0xe0&&d<0xa0)||(c==0xed&&d>=0xa0)||(c==0xf0&&d<0x90)||(c==0xf4&&d>=0x90))return false;}
            i+=n;
        }
        return false;
    }
    bool valid(bool older=false)const {
        if(version!=kVersion||mode>Custom||!pageCount||pageCount>kPages||catalogCount>kFunctions.size()||(!older&&catalogCount!=kFunctions.size())||!catalogCount)return false;
        uint64_t known=0;for(auto app:kFunctions)known|=uint64_t{1}<<static_cast<unsigned>(app);
        for(unsigned i=0;i<kApps;++i)if((hidden[i/8]&(1u<<(i%8)))&&(!(known&(uint64_t{1}<<i))||i==static_cast<unsigned>(App::Settings)))return false;
        unsigned groupBits=0,pageBits=0;
        for(auto group:categories){if(group>=5||(groupBits&(1u<<group)))return false;groupBits|=1u<<group;}
        for(unsigned i=0;i<pageCount;++i){if(pages[i]>=pageCount||(pageBits&(1u<<pages[i]))||!textValid(names[i]))return false;pageBits|=1u<<pages[i];}
        uint64_t first=0;
        for(unsigned modeIndex=0;modeIndex<3;++modeIndex){
            uint64_t seen=0;
            for(unsigned i=0;i<catalogCount;++i){const auto id=order[modeIndex][i];if(id>=kApps||!(known&(uint64_t{1}<<id))||(seen&(uint64_t{1}<<id)))return false;seen|=uint64_t{1}<<id;}
            if(!modeIndex)first=seen;else if(seen!=first)return false;
        }
        if(!(first&(uint64_t{1}<<static_cast<unsigned>(App::Settings))))return false;
        unsigned counts[kPages]{};
        for(auto app:kFunctions){const unsigned id=static_cast<unsigned>(app);if(!(first&(uint64_t{1}<<id)))continue;if(assignment[id]>=pageCount)return false;if(shown(app)&&++counts[assignment[id]]>9)return false;}
        return true;
    }
    bool upgrade() {
        if(!valid(true))return false;
        if(catalogCount==kFunctions.size())return true;
        for(auto app:kFunctions){const auto id=static_cast<uint8_t>(app);bool found=false;for(unsigned i=0;i<catalogCount;++i)found|=order[0][i]==id;if(found)continue;
            unsigned counts[kPages]{};for(unsigned i=0;i<catalogCount;++i)if(shown(static_cast<App>(order[0][i])))++counts[assignment[order[0][i]]];
            unsigned page=0;while(page<pageCount&&counts[page]>=9)++page;
            if(page==pageCount){if(pageCount==kPages)return false;pages[pageCount]=pageCount;strcpy(names[pageCount++],"新增功能");}
            assignment[id]=page;show(app,true);for(auto& list:order)list[catalogCount]=id;++catalogCount;
        }
        return valid();
    }
};
static_assert(sizeof(MenuLayout)==MenuLayout::kBytes);
static_assert(static_cast<unsigned>(App::Count)<=MenuLayout::kApps&&kFunctions.size()<=MenuLayout::kApps);
inline MenuLayout defaultMenuLayout(){
    MenuLayout layout;std::memset(layout.order,255,sizeof(layout.order));
    for(unsigned i=0;i<MenuLayout::kPages;++i)layout.pages[i]=i;
    // Short names fit the custom-page eight-character limit (the original category titles can be longer).
    const char* names[]{"小智语音","电脑时间","天气财经资讯","学习影音","工具设置"};
    for(unsigned i=0;i<5;++i)std::strcpy(layout.names[i],names[i]);
    for(unsigned i=0;i<kFunctions.size();++i){const auto app=kFunctions[i];for(auto& list:layout.order)list[i]=static_cast<uint8_t>(app);layout.assignment[static_cast<unsigned>(app)]=menuPageOf(i);}
    return layout;
}
struct MenuView {
    uint8_t items[MenuLayout::kApps]{},starts[MenuLayout::kPages+1]{},groups[MenuLayout::kPages]{},count{},pages{};
    void build(const MenuLayout& layout){
        count=pages=0;
        const unsigned groups=layout.mode==MenuLayout::Continuous?1:layout.mode==MenuLayout::Categories?5:layout.pageCount;
        for(unsigned p=0;p<groups;++p){
            const unsigned group=layout.mode==MenuLayout::Categories?layout.categories[p]:layout.mode==MenuLayout::Custom?layout.pages[p]:0;
            const auto begin=count;
            for(unsigned i=0;i<layout.catalogCount;++i){const auto app=static_cast<App>(layout.order[layout.mode][i]);if(!layout.shown(app))continue;
                if(layout.mode==MenuLayout::Categories&&menuCategory(app)!=group)continue;
                if(layout.mode==MenuLayout::Custom&&layout.assignment[static_cast<unsigned>(app)]!=group)continue;
                const int index=menuCatalogIndex(app);if(index>=0)items[count++]=static_cast<uint8_t>(index);
            }
            if(count!=begin){starts[pages]=begin;this->groups[pages++]=group;}
        }
        starts[pages]=count;
    }
    unsigned position(unsigned catalogIndex)const {for(unsigned i=0;i<count;++i)if(items[i]==catalogIndex)return i;return count;}
    unsigned page(unsigned position)const {unsigned p=0;while(p+1<pages&&position>=starts[p+1])++p;return p;}
};
}
