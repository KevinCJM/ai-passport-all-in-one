#include "native_services.hpp"
#include "macro_json.hpp"
#include <memory>
#include <ctime>

namespace passport {
namespace {
// FRED's CSV delivery stalls on the SDK's default User-Agent. This curl-compatible
// identifier (including our product name) is only used for public macro downloads.
constexpr char kMacroDownloadAgent[]="curl/8.7.1 AI-Passport";
constexpr const char* kCaches[]{"macro_cn_cache","macro_cache","macro_cm_cache"};
constexpr const char* kEtfHeaders[]{"x-app-id","rU6QIu7JHe2gOUeR","x-csrf-token","x-csrf-token","x-version","1.0.0",nullptr};
struct LegacyMacroView:MacroView{static constexpr uint32_t kVersion=1;};
void jsonUrl(char* out,size_t size,uint8_t series,bool chart,uint32_t before){
    const auto& spec=kMacroSeries[series];
    if(spec.source==MacroSource::Oil){snprintf(out,size,"https://cdn.jin10.com/data_center/reports/usa_oil.json");return;}
    if(spec.source==MacroSource::Etf){char date[16]{};if(before){tm t{};t.tm_year=before/10000-1900;t.tm_mon=before/100%100-1;t.tm_mday=before%100-1;timegm(&t);strftime(date,sizeof(date),"%Y-%m-%d",&t);}
        snprintf(out,size,"https://datacenter-api.jin10.com/reports/list_v2?category=etf&attr_id=%s&max_date=%s",spec.filter,date);return;}
    const char* date=series==19?"TRADE_DATE":"REPORT_DATE";
    char filter[96]{};if(spec.filter)snprintf(filter,sizeof(filter),"&filter=%%28INDICATOR_ID%%3D%%22%s%%22%%29",spec.filter);
    snprintf(out,size,"https://datacenter-web.eastmoney.com/api/data/v1/get?reportName=%s&columns=%s,%s&sortColumns=%s&sortTypes=-1&pageNumber=1&pageSize=%u&source=WEB&client=WEB%s",spec.report,date,spec.field,date,chart?60:2,filter);
}

}
void NativeServices::macro(){
    unsigned group=1,detail=0;char verb[8]{};
    if(sscanf(current_.operation,"%7[^:]:%u:%u",verb,&group,&detail)<2||group>2||(strcmp(verb,"list")&&strcmp(verb,"chart"))){publish(current_,"分类参数错误","error");return;}
    const MacroBoard board(group);
    auto cached=std::unique_ptr<MacroView>(new(std::nothrow)MacroView);
    if(!cached){publish(current_,"内存不足","error");return;}
    bool saved=mediaLoad(kCaches[group],*cached);
    if(!saved&&group==1){LegacyMacroView old;if(mediaLoad("macro_cache",old)){*cached=old;cached->version=MacroView::kVersion;cached->group=1;saved=true;}}
    saved=saved&&cached->valid()&&cached->group==group;
    auto& view=page_.macro;view=MacroView{};view.group=group;view.count=board.count;
    for(unsigned i=0;i<board.count;++i){
        view.rows[i].series=board.items[i];
        if(saved)for(unsigned j=0;j<cached->count;++j)if(cached->rows[j].series==board.items[i])view.rows[i]=cached->rows[j];
    }
    view.updated=saved?cached->updated:0;
    if(saved&&cached->points&&cached->chartFor<cached->count)for(unsigned i=0;i<view.count;++i)if(view.rows[i].series==cached->rows[cached->chartFor].series){
        view.chartFor=i;view.points=cached->points;view.chart=cached->chart;view.low=cached->low;view.high=cached->high;view.from=cached->from;view.zoomFrom=cached->zoomFrom;view.to=cached->to;break;
    }
    if(!network_){publish(current_,"离线 · 上次数据","error");return;}
    publish(current_,"正在更新宏观数据","loading");
    if(!syncClock()){publish(current_,"校时失败，请重试","error");return;}
    const bool chart=!strcmp(verb,"chart")&&detail<board.count;
    const unsigned start=chart?detail:0,end=chart?detail+1:board.count;
    bool changed=!saved||cached->count!=board.count;unsigned good=0;const char* failure=nullptr;
    for(unsigned i=start;i<end&&!cancelled();++i){
        const auto series=board.items[i];const auto& spec=kMacroSeries[series];
        bool fetched=false,parsed=false;MacroRow next{};
        if(spec.source==MacroSource::Fred){
            const time_t now=time(nullptr);tm since{};gmtime_r(&now,&since);
            if(!strcmp(spec.frequency,"日频"))since.tm_mday-=chart?150:30;
            else if(!strcmp(spec.frequency,"周频"))since.tm_mday-=chart?500:40;
            else if(!strcmp(spec.frequency,"季频"))since.tm_year-=chart?16:2;
            else since.tm_mon-=chart?84:16;
            timegm(&since);char date[16];strftime(date,sizeof(date),"%Y-%m-%d",&since);
            char url[192];snprintf(url,sizeof(url),"https://fred.stlouisfed.org/graph/fredgraph.csv?id=%s&cosd=%s",spec.id,date);
            auto parser=std::unique_ptr<MacroCsv>(new(std::nothrow)MacroCsv(series));
            if(!parser){publish(current_,"内存不足","error");return;}
            struct Read{MacroCsv* parser;uint8_t series;} read{parser.get(),series};
            const Sink resetting=[](void* context,const char* bytes,size_t size){auto& r=*static_cast<Read*>(context);if(!bytes)*r.parser=MacroCsv(r.series);else r.parser->feed(bytes,size);return true;};
            fetched=fetch(url,nullptr,0,resetting,&read,nullptr,kMacroDownloadAgent);
            parsed=fetched&&parser->finish();if(parsed){next=parser->row();if(chart)parser->chart(view,i);}
        }else{
            auto parser=std::unique_ptr<MacroJson>(new(std::nothrow)MacroJson(series));
            auto points=std::unique_ptr<MacroPoints>(new(std::nothrow)MacroPoints);
            if(!parser||!points){publish(current_,"内存不足","error");return;}
            struct Read{MacroJson* parser;uint8_t series;} read{parser.get(),series};
            const Sink resetting=[](void* context,const char* bytes,size_t size){auto& r=*static_cast<Read*>(context);if(!bytes)*r.parser=MacroJson(r.series);else r.parser->feed(bytes,size);return true;};
            uint32_t before=0;const unsigned pages=spec.source==MacroSource::Etf&&chart?3:1;
            for(unsigned page=0;page<pages&&!cancelled();++page){
                char url[512];jsonUrl(url,sizeof(url),series,chart,before);
                fetched=fetch(url,nullptr,0,resetting,&read,nullptr,kMacroDownloadAgent,spec.source==MacroSource::Etf?kEtfHeaders:nullptr);
                parsed=fetched&&parser->finish()&&points->merge(parser->points);if(!parsed)break;
                if(before&&parser->oldest>=before){parsed=false;break;}before=parser->oldest;
            }
            if(parsed){next=points->row(series);if(chart)points->chart(view,i);}
        }
        if(parsed){
            const auto& old=view.rows[i];
            const auto same=[](float a,float b){return a==b||(std::isnan(a)&&std::isnan(b));};
            changed|=old.series!=next.series||old.date!=next.date||!same(old.value,next.value)||!same(old.previous,next.previous);
            view.rows[i]=next;++good;if(chart)changed=true;
        }else if(!failure){
            const auto code=httpStatus_.load();const int error=error_.load();
            if(fetched)failure="数据格式异常";
            else if(error==ESP_ERR_NO_MEM)failure="内存不足";
            else if(tlsFlags_.load())failure="证书校验失败";
            else if(code==401||code==403)failure="数据源拒绝访问";
            else if(code==429)failure="请求频繁，请稍后";
            else if(code>=500&&code<600)failure="数据源暂不可用";
            else if(error==ESP_ERR_HTTP_EAGAIN||error==ESP_ERR_HTTP_READ_TIMEOUT)failure="数据源响应超时";
            else failure="网络请求失败";
        }
        if(!cancelled()){char state[48];snprintf(state,sizeof(state),"宏观数据 %u/%u",i-start+1,end-start);publish(current_,state,"loading");}
    }
    if(cancelled())return;
    if(good==end-start)view.updated=time(nullptr);
    if(changed&&good&&!mediaSave(kCaches[group],view))snprintf(notice_,sizeof(notice_),"已更新，缓存保存失败");
    if(good==end-start)publish(current_,group==1?"已更新 · FRED":group==0?"已更新 · 东方财富":"已更新 · 商品数据","ready");
    else{char state[96];snprintf(state,sizeof(state),good?"部分更新 · %s":saved?"%s · 显示缓存":"%s，请重试",failure?failure:"获取失败");publish(current_,state,"error");}
}
}
