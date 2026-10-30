#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace passport {
enum class MacroTransform : uint8_t { Level, YoY, MonthlyChange };
enum class MacroSource : uint8_t { Fred, Eastmoney, Oil, Etf };
inline constexpr const char* kMacroGroups[]{"中国","美国","商品"};
inline constexpr uint32_t kMacroColors[]{0x4bffc4,0x63adff,0xffc76a};
inline constexpr uint32_t kMacroPanels[]{0x124135,0x163650,0x45351e};
struct MacroSeries {
    const char *id, *name, *aliases, *frequency, *unit;
    MacroTransform transform; float scale; bool basisPoints;
    MacroSource source{MacroSource::Fred};uint8_t group{1};
    const char *report{},*field{},*filter{},*note{};
};
// Display units and transformations are explicit: CPI is the unadjusted index's year-on-year change.
inline constexpr MacroSeries kMacroSeries[]{
    {"DFF","联邦基金利率","联邦基金|联邦利率|美联储利率|美国利率|federalfunds","日频","%",MacroTransform::Level,1,true},
    {"DGS2","两年美债","两年美债|二年美债|2年美债|两年期美债|两年期美国国债","日频","%",MacroTransform::Level,1,true},
    {"DGS10","十年美债","十年美债|10年美债|十年期美债|十年期美国国债","日频","%",MacroTransform::Level,1,true},
    {"T10Y2Y","美债期限利差","期限利差|美债利差|十年减两年|收益率曲线","日频","bp",MacroTransform::Level,100,false},
    {"CPIAUCNS","美国CPI同比","美国cpi|cpi|美国通胀|消费者物价","月频","%",MacroTransform::YoY,1,false},
    {"UNRATE","美国失业率","美国失业率|失业率|unemployment","月频","%",MacroTransform::Level,1,false},
    {"CPILFENS","核心CPI同比","核心cpi|核心通胀","月频","%",MacroTransform::YoY,1,false},
    {"PCEPILFE","核心PCE同比","核心pce|核心个人消费支出","月频","%",MacroTransform::YoY,1,false},
    {"PAYEMS","非农新增就业","非农|新增就业|非农业就业","月频","万人",MacroTransform::MonthlyChange,0.1f,false},
    {"ICSA","初请失业金","初请|初次申请失业金|失业金人数","周频","千人",MacroTransform::Level,0.001f,false},
    {"A191RL1Q225SBEA","实际GDP增速","美国gdp|实际gdp|gdp|国内生产总值","季频","%",MacroTransform::Level,1,false},
    {"DTWEXBGS","美元广义指数","美元广义|美元贸易加权|广义美元","日频","点",MacroTransform::Level,1,false},
    {"M2SL","美国M2同比","美国m2|m2|货币供应","月频","%",MacroTransform::YoY,1,false},
    {"DGS30","三十年美债","三十年美债|30年美债|三十年期美债","日频","%",MacroTransform::Level,1,true},
    {"CN_GDP","GDP累计同比","中国gdp|中国国内生产总值|GDP累计同比","季频","%",MacroTransform::Level,1,false,MacroSource::Eastmoney,0,"RPT_ECONOMY_GDP","SUM_SAME",nullptr,"年初至今累计同比"},
    {"CN_CPI","中国CPI同比","中国cpi|中国通胀|中国消费者物价","月频","%",MacroTransform::Level,1,false,MacroSource::Eastmoney,0,"RPT_ECONOMY_CPI","NATIONAL_SAME",nullptr,"全国居民消费价格同比"},
    {"CN_PPI","中国PPI同比","中国ppi|ppi|工业生产者价格","月频","%",MacroTransform::Level,1,false,MacroSource::Eastmoney,0,"RPT_ECONOMY_PPI","BASE_SAME",nullptr,"工业生产者出厂价格同比"},
    {"CN_PMI","制造业PMI","中国pmi|pmi|制造业pmi|制造业采购经理","月频","点",MacroTransform::Level,1,false,MacroSource::Eastmoney,0,"RPT_ECONOMY_PMI","MAKE_INDEX",nullptr,"制造业采购经理指数"},
    {"CN_M2","中国M2同比","中国m2|中国货币供应","月频","%",MacroTransform::Level,1,false,MacroSource::Eastmoney,0,"RPT_ECONOMY_CURRENCY_SUPPLY","BASIC_CURRENCY_SAME",nullptr,"广义货币供应量同比"},
    {"CN_LPR","一年期LPR","中国lpr|lpr|一年期lpr|贷款市场报价利率","月频","%",MacroTransform::Level,1,true,MacroSource::Eastmoney,0,"RPTA_WEB_RATE","LPR1Y",nullptr,"一年期贷款市场报价利率"},
    {"COM_PRICE","大宗商品价格","大宗商品|商品价格指数","日频","点",MacroTransform::Level,1,false,MacroSource::Eastmoney,2,"RPT_INDUSTRY_INDEX","INDICATOR_VALUE","EMI00662535","大宗商品价格指数"},
    {"BDI","BDI干散货","bdi|波罗的海|干散货|航运指数","日频","点",MacroTransform::Level,1,false,MacroSource::Eastmoney,2,"RPT_INDUSTRY_INDEX","INDICATOR_VALUE","EMI00107664","波罗的海干散货运价指数"},
    {"US_OIL","美国原油产量","美国原油产量|原油产量|美国石油产量","周频","万桶/日",MacroTransform::Level,1,false,MacroSource::Oil,2,nullptr,nullptr,nullptr,"美国国内原油总产量"},
    {"GOLD_ETF","黄金ETF持仓","黄金etf|黄金持仓|黄金库存","日频","吨",MacroTransform::Level,1,false,MacroSource::Etf,2,nullptr,nullptr,"1","SPDR黄金ETF总持仓"},
    {"SILVER_ETF","白银ETF持仓","白银etf|白银持仓|白银库存","日频","吨",MacroTransform::Level,1,false,MacroSource::Etf,2,nullptr,nullptr,"2","白银ETF总持仓"},
    {"AGRI_PRICE","农产品批发价","农产品|农产品批发|农业价格","日频","点",MacroTransform::Level,1,false,MacroSource::Eastmoney,2,"RPT_INDUSTRY_INDEX","INDICATOR_VALUE","EMI00009274","农产品批发价格总指数"},
};
struct MacroBoard {
    static constexpr uint32_t kVersion=2; static constexpr unsigned kMax=12;
    uint32_t version{kVersion}; uint8_t count{6}; uint8_t items[kMax]{};uint8_t group{1};
    explicit MacroBoard(unsigned category=1):group(category<3?category:1){
        constexpr uint8_t defaults[3][6]{{14,15,16,17,18,19},{0,2,4,5,8,10},{20,21,22,23,24,25}};
        std::copy(defaults[group],defaults[group]+6,items);
    }
    bool valid() const {
        if(version!=kVersion||group>2||count!=6)return false;
        const MacroBoard fixed(group);for(unsigned i=0;i<count;++i)if(items[i]!=fixed.items[i])return false;
        for(unsigned i=0;i<count;++i){if(items[i]>=std::size(kMacroSeries)||kMacroSeries[items[i]].group!=group)return false;for(unsigned j=0;j<i;++j)if(items[i]==items[j])return false;}
        return true;
    }
};
struct MacroRow {uint8_t series{};float value{NAN},previous{NAN};uint32_t date{};};
struct MacroView {
    static constexpr uint32_t kVersion=2;
    uint32_t version{kVersion},updated{};uint8_t count{},chartFor{255},points{},group{1};
    std::array<MacroRow,MacroBoard::kMax> rows{};
    std::array<int16_t,60> chart{};float low{},high{};uint32_t from{},zoomFrom{},to{};
    bool valid() const {
        if(version!=kVersion||group>2||count>rows.size()||points>chart.size()||(chartFor!=255&&chartFor>=count))return false;
        for(unsigned i=0;i<count;++i)if(rows[i].series>=std::size(kMacroSeries)||kMacroSeries[rows[i].series].group!=group||(!std::isfinite(rows[i].value)&&rows[i].date))return false;
        if(points&&(!std::isfinite(low)||!std::isfinite(high)||high<low||!from||to<from))return false;
        for(unsigned i=0;i<points;++i)if(chart[i]<0||chart[i]>999)return false;
        return true;
    }
};
inline float macroDelta(const MacroRow& row){const auto& s=kMacroSeries[row.series];return (row.value-row.previous)*(s.basisPoints?100:1);}
inline const char* macroDeltaUnit(const MacroSeries& s){return s.basisPoints||!strcmp(s.unit,"bp")?"bp":!strcmp(s.unit,"%")?"百分点":s.unit;}
inline void macroDate(char* out,size_t size,uint32_t date){if(date)snprintf(out,size,"%04u-%02u-%02u",unsigned(date/10000),unsigned(date/100%100),unsigned(date%100));else snprintf(out,size,"--");}
// Incremental, bounded CSV reader. Empty holiday observations never become zero; monthly transforms require the exact month.
class MacroCsv {
public:
    explicit MacroCsv(uint8_t series):series_(series){}
    void feed(const char* data,size_t size){for(size_t i=0;i<size;++i){if(data[i]=='\n'){line();used_=0;}else if(data[i]!='\r'){if(used_+1<sizeof(line_))line_[used_++]=data[i];else bad_=true;}}}
    bool finish(){if(used_)line();used_=0;return !bad_&&header_&&count_;}
    MacroRow row() const{return {series_,count_?values_[count_-1]:NAN,count_>1?values_[count_-2]:NAN,count_?dates_[count_-1]:0};}
    void chart(MacroView& view,unsigned row) const{
        view.chartFor=row;view.points=count_;if(!count_)return;
        const auto bounds=std::minmax_element(values_,values_+count_);view.low=*bounds.first;view.high=*bounds.second;
        view.from=dates_[0];view.zoomFrom=dates_[count_>12?count_-12:0];view.to=dates_[count_-1];
        for(unsigned i=0;i<count_;++i)view.chart[i]=view.high>view.low?int16_t(std::lround((values_[i]-view.low)/(view.high-view.low)*999)):500;
    }
private:
    uint8_t series_;char line_[96]{};unsigned used_{},count_{};bool bad_{},header_{};uint32_t last_{};
    float values_[60]{},history_[13]{};uint32_t dates_[60]{},months_[13]{};
    void line(){
        line_[used_]=0;if(!used_)return;
        if(!header_){char expected[64];snprintf(expected,sizeof(expected),"observation_date,%s",kMacroSeries[series_].id);header_=!strcmp(line_,expected);if(!header_)bad_=true;return;}
        unsigned y,m,d;int n=0;
        if(sscanf(line_,"%4u-%2u-%2u,%n",&y,&m,&d,&n)!=3||n!=11||y<1900||y>2200||m<1||m>12){bad_=true;return;}
        constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};const unsigned max=days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));
        if(!d||d>max){bad_=true;return;}const uint32_t date=y*10000+m*100+d;if(date<=last_){bad_=true;return;}last_=date;
        const char* at=line_+n;char* end;float raw=std::strtof(at,&end);const bool missing=!*at||!strcmp(at,".");
        if(!missing&&(end==at||*end||!std::isfinite(raw))){bad_=true;return;}if(missing)raw=NAN;
        const auto& s=kMacroSeries[series_];const unsigned month=y*12+m,slot=month%13;float value=raw;
        if(s.transform!=MacroTransform::Level){const unsigned ago=s.transform==MacroTransform::YoY?12:1,prior=(month-ago)%13;
            value=months_[prior]==month-ago&&std::isfinite(history_[prior])?(s.transform==MacroTransform::YoY?(history_[prior]!=0?(raw/history_[prior]-1)*100:NAN):raw-history_[prior]):NAN;
            history_[slot]=raw;months_[slot]=month;}
        value*=s.scale;if(!std::isfinite(value))return;
        if(count_==60){std::move(values_+1,values_+60,values_);std::move(dates_+1,dates_+60,dates_);--count_;}
        values_[count_]=value;dates_[count_++]=date;
    }
};
}
