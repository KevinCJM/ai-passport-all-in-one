"""Compile and exercise the same bounded macro parser used by the ESP32."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class MacroTests(unittest.TestCase):
    def test_native_download_errors_and_cache(self):
        # Compile the real macro service with fake HTTP/NVS boundaries. This catches
        # a missing endpoint-specific User-Agent and failure paths that erase data.
        stub = r'''
#pragma once
#include "macro.hpp"
#include <atomic>
#include <string>
#include <memory>
#include <type_traits>
#include <fstream>
#include <cassert>
#include <map>
#include <vector>
constexpr int ESP_ERR_NO_MEM=0x101,ESP_ERR_HTTP_EAGAIN=0x7007,ESP_ERR_HTTP_READ_TIMEOUT=0x700b;
namespace passport {
inline MacroBoard storedBoard;
inline MacroView storedCache;
inline bool haveCache=false;
inline unsigned saves=0;
inline std::map<std::string,std::vector<char>> extra;
template<class T> bool mediaLoad(const char* key,T& value){
    (void)T::kVersion;
    if(std::string(key)!="macro_cache"&&std::string(key)!="macro_us_list"){auto at=extra.find(key);if(at==extra.end()||at->second.size()!=sizeof(T))return false;memcpy(&value,at->second.data(),sizeof(T));return value.version==T::kVersion;}
    if constexpr(std::is_same_v<T,MacroBoard>){value=storedBoard;return true;}
    else if constexpr(std::is_base_of_v<MacroView,T>){static_cast<MacroView&>(value)=storedCache;return haveCache&&value.version==T::kVersion;}
    else return false;
}
template<class T> bool mediaSave(const char* key,const T& value){
    if(std::string(key)!="macro_cache"&&std::string(key)!="macro_us_list"){const char* bytes=reinterpret_cast<const char*>(&value);extra[key]=std::vector<char>(bytes,bytes+sizeof(T));++saves;return true;}
    ++saves;if constexpr(std::is_same_v<T,MacroBoard>)storedBoard=value;
    else if constexpr(std::is_same_v<T,MacroView>){storedCache=value;haveCache=true;}return true;
}
class NativeServices {
public:
    struct Job{char operation[16]{"list:1"};} current_;
    struct Page{MacroView macro;} page_;
    char notice_[160]{};int select_=-1;bool network_=true,clock=true;
    std::atomic<unsigned> httpStatus_{};std::atomic<int> error_{},tlsFlags_{};
    int failureCode=0,failureStatus=-1;bool malformed=false,restartSink=false;
    std::string failedSeries,state,phase;unsigned calls=0;
    using Sink=bool(*)(void*,const char*,size_t);
    bool cancelled()const{return false;}
    bool syncClock(){return clock;}
    void publish(const Job&,const char* s,const char* p="ready"){state=s;phase=p;}
    void status(const char* s){state=s;}
    bool fetch(const char* url,char*,size_t,Sink sink=nullptr,void* context=nullptr,const char* post=nullptr,const char* agent=nullptr,const char* const* headers=nullptr){
        ++calls;
        // Reproduce the observed server behavior for the SDK default agent.
        if(!agent||!strstr(agent,"curl/")||!strstr(agent,"AI-Passport")){httpStatus_=-1;error_=ESP_ERR_HTTP_EAGAIN;return false;}
        if(post)std::abort();
        if(failureCode||failureStatus>0){
            if(failedSeries.empty()||strstr(url,failedSeries.c_str())){httpStatus_=failureStatus;error_=failureCode;return false;}
        }
        httpStatus_=200;error_=0;
        std::string body;
        if(const char* found=strstr(url,"?id=")){
        const char* id=found+4;std::string series(id,strchr(id,'&')-id);
        body="observation_date,"+series+"\n";
        body+=series=="PAYEMS"?"2026-08-01,100000\n2026-09-01,100250\n":series=="CPIAUCNS"?"2024-09-01,100\n2024-10-01,200\n2025-09-01,105\n2025-10-01,212\n":"2026-10-26,3.8\n2026-10-27,3.9\n";
        }else{
            int series=-1;
            for(int i=14;i<26;++i){const auto& s=kMacroSeries[i];
                if(s.source==MacroSource::Oil&&strstr(url,"usa_oil.json"))series=i;
                if(s.source==MacroSource::Eastmoney&&strstr(url,s.report)&&strstr(url,s.field)&&(!s.filter||strstr(url,s.filter)))series=i;
                if(s.source==MacroSource::Etf&&strstr(url,(std::string("attr_id=")+s.filter).c_str())){series=i;assert(headers&&std::string(headers[0])=="x-app-id");}
            }
            assert(series>=14);std::string file="@FIXTURES@/"+std::to_string(series)+".json";
            if(kMacroSeries[series].source==MacroSource::Etf){const char* date=strstr(url,"max_date=")+9;if(*date)file="@FIXTURES@/pages/"+std::to_string(series)+"-"+date+".json";}
            std::ifstream input(file);body.assign(std::istreambuf_iterator<char>(input),{});assert(!body.empty());
        }
        if(malformed)body="<html>unavailable</html>";
        if(restartSink){sink(context,nullptr,0);sink(context,"partial error response",22);}
        sink(context,nullptr,0);
        for(size_t i=0;i<body.size();i+=7)sink(context,body.data()+i,std::min<size_t>(7,body.size()-i));
        return true;
    }
    void macro();
};
}
'''
        source = r'''
#include "native_services.hpp"
#include <cassert>
using namespace passport;
void reset(){storedBoard=MacroBoard{};storedCache={};haveCache=false;saves=0;extra.clear();}
int main(){
    reset();NativeServices good;good.restartSink=true;good.macro();
    assert(good.phase=="ready"&&good.calls==6&&saves==1&&haveCache);
    assert(good.page_.macro.updated&&good.page_.macro.count==6);
    for(unsigned i=0;i<6;++i)assert(std::isfinite(good.page_.macro.rows[i].value)&&good.page_.macro.rows[i].date);
    assert(std::fabs(good.page_.macro.rows[2].value-6)<0.002f);
    const MacroView cached=storedCache;
    storedCache.updated=123;saves=0;NativeServices same;same.macro();
    assert(same.phase=="ready"&&saves==0&&storedCache.updated==123);
    saves=0;NativeServices chart;strcpy(chart.current_.operation,"chart:1:0");chart.macro();
    assert(chart.phase=="ready"&&chart.calls==1&&chart.page_.macro.chartFor==0&&chart.page_.macro.points==2&&saves==1);
    for(int error:{ESP_ERR_HTTP_EAGAIN,ESP_ERR_HTTP_READ_TIMEOUT}){
        storedCache=cached;storedCache.updated=123;saves=0;NativeServices timeout;timeout.failureCode=error;timeout.macro();
        assert(timeout.phase=="error"&&timeout.state.find("数据源响应超时")!=std::string::npos&&timeout.state.find("显示缓存")!=std::string::npos);
        assert(timeout.page_.macro.updated==123&&timeout.page_.macro.rows[0].value==cached.rows[0].value&&saves==0);
    }
    reset();NativeServices empty;empty.failureCode=ESP_ERR_HTTP_EAGAIN;empty.macro();
    assert(empty.state.find("请重试")!=std::string::npos&&!haveCache&&saves==0&&!empty.page_.macro.updated);
    storedCache=cached;storedCache.updated=123;haveCache=true;saves=0;
    NativeServices partial;partial.failureCode=ESP_ERR_HTTP_EAGAIN;partial.failedSeries="id=DGS10&";partial.macro();
    assert(partial.phase=="error"&&partial.state.find("部分更新")!=std::string::npos&&partial.page_.macro.updated==123);
    assert(partial.page_.macro.rows[1].value==cached.rows[1].value);
    for(int code:{403,429,503}){
        NativeServices rejected;rejected.failureStatus=code;rejected.macro();
        const char* message=code==403?"拒绝访问":code==429?"请求频繁":"暂不可用";
        assert(rejected.phase=="error"&&rejected.state.find(message)!=std::string::npos);
    }
    NativeServices invalid;invalid.malformed=true;invalid.macro();
    assert(invalid.phase=="error"&&invalid.state.find("数据格式异常")!=std::string::npos&&invalid.page_.macro.rows[0].value==cached.rows[0].value);
    NativeServices offline;offline.network_=false;offline.macro();assert(offline.calls==0&&offline.phase=="error");
    NativeServices clock;clock.clock=false;clock.macro();assert(clock.calls==0&&clock.state.find("校时失败")!=std::string::npos);
    reset();NativeServices cn;strcpy(cn.current_.operation,"list:0");cn.restartSink=true;cn.macro();assert(cn.phase=="ready"&&cn.calls==6&&cn.page_.macro.valid()&&cn.page_.macro.rows[0].series==14&&extra.count("macro_cn_cache"));
    NativeServices cm;strcpy(cm.current_.operation,"list:2");cm.macro();assert(cm.phase=="ready"&&cm.calls==6&&cm.page_.macro.valid()&&cm.page_.macro.rows[0].series==20&&extra.count("macro_cm_cache"));
    for(int row:{3,4}){NativeServices etf;snprintf(etf.current_.operation,16,"chart:2:%d",row);etf.macro();assert(etf.phase=="ready"&&etf.calls==3&&etf.page_.macro.points==60&&etf.page_.macro.chartFor==row);}
    NativeServices oil;strcpy(oil.current_.operation,"chart:2:2");oil.macro();assert(oil.phase=="ready"&&oil.calls==1&&oil.page_.macro.points==60&&oil.page_.macro.rows[2].value>1000);
    NativeServices offcn;strcpy(offcn.current_.operation,"list:0");offcn.network_=false;offcn.macro();assert(offcn.calls==0&&offcn.page_.macro.rows[0].series==14&&std::isfinite(offcn.page_.macro.rows[0].value));
    NativeServices invalidOp;strcpy(invalidOp.current_.operation,"del:0:1");invalidOp.macro();assert(invalidOp.phase=="error"&&invalidOp.calls==0);
    storedCache=cached;storedCache.version=1;storedCache.group=0;haveCache=true;NativeServices migrated;migrated.network_=false;migrated.macro();assert(migrated.page_.macro.valid()&&migrated.page_.macro.group==1&&migrated.page_.macro.rows[0].value==cached.rows[0].value);
    struct Legacy{uint32_t version=1;uint8_t count=7,items[12]{0,1,2,3,4,5,13};} old;
    const char* bytes=reinterpret_cast<const char*>(&old);extra["macro_list"]=std::vector<char>(bytes,bytes+sizeof(old));
    storedBoard.version=0;NativeServices added;added.network_=false;added.macro();assert(added.page_.macro.count==6&&added.page_.macro.rows[5].series==10);
}
'''
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="passport-macro-native-") as directory:
            path = Path(directory)
            (path / "native_services.hpp").write_text(stub.replace("@FIXTURES@",str(ROOT/"tests/fixtures/macro")))
            (path / "macro_test.cpp").write_text(source)
            shutil.copyfile(ROOT / "firmware/passport_device/main/macro_services.cpp",
                            path / "macro_services.cpp")
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            "-I", str(ROOT / "firmware/passport_suite/main"),
                            str(path / "macro_test.cpp"), str(path / "macro_services.cpp"),
                            "-o", str(path / "macro_test")], check=True)
            subprocess.run([str(path / "macro_test")], check=True)

    def test_csv_math_readonly_ui_contracts(self):
        source = r'''
#include "macro.hpp"
#include "key_hints.hpp"
#include <cassert>
#include <string>
using namespace passport;
bool close(float a,float b){return std::fabs(a-b)<0.002f;}
void chunks(MacroCsv& csv,const std::string& text,unsigned size){for(size_t i=0;i<text.size();i+=size)csv.feed(text.data()+i,std::min<size_t>(size,text.size()-i));}
int main(){
    static_assert(sizeof(MacroCsv)<1024&&sizeof(MacroView)<512);
    MacroBoard board;assert(board.valid());board.items[1]=board.items[0];assert(!board.valid());board=MacroBoard{};board.count=13;assert(!board.valid());
    for(unsigned group=0;group<3;++group){MacroBoard fixed(group);assert(fixed.valid()&&fixed.count==6);fixed.count=5;assert(!fixed.valid());}
    const std::string daily="observation_date,DGS10\r\n2026-10-01,4.20\r\n2026-10-02,\r\n2026-10-05,.\r\n2026-10-06,4.25";
    for(unsigned n=1;n<32;++n){MacroCsv csv(2);chunks(csv,daily,n);assert(csv.finish());auto row=csv.row();assert(row.date==20261006&&close(row.value,4.25)&&close(row.previous,4.2)&&close(macroDelta(row),5));MacroView v;v.count=1;v.rows[0]=row;csv.chart(v,0);assert(v.valid()&&v.points==2&&v.chart[0]==0&&v.chart[1]==999);}
    {MacroCsv csv(4);chunks(csv,"observation_date,CPIAUCNS\n2024-01-01,100\n2024-02-01,200\n2025-01-01,105\n2025-02-01,212\n2025-03-01,110\n",3);assert(csv.finish());auto r=csv.row();assert(r.date==20250201&&close(r.value,6)&&close(r.previous,5)&&close(macroDelta(r),1));}
    {MacroCsv csv(4);chunks(csv,"observation_date,CPIAUCNS\n2024-01-01,0\n2025-01-01,100\n",1);assert(!csv.finish());}
    {MacroCsv csv(8);chunks(csv,"observation_date,PAYEMS\n2026-01-01,100000\n2026-02-01,100250\n2026-04-01,100500\n",7);assert(csv.finish());assert(csv.row().date==20260201&&close(csv.row().value,25));}
    for(const auto& bad:{"observation_date,WRONG\n2026-01-01,1\n","<html>blocked</html>","observation_date,DGS10\n2026-02-30,1\n","observation_date,DGS10\n2026-01-01,nan\n","observation_date,DGS10\n2026-01-01,1junk\n","observation_date,DGS10\n2026-01-02,1\n2026-01-01,2\n"}){MacroCsv csv(2);chunks(csv,bad,1);assert(!csv.finish());}
    {MacroCsv csv(2);chunks(csv,"observation_date,DGS10\n2026-01-01,"+std::string(100,'1')+"\n",11);assert(!csv.finish());}
    {MacroCsv csv(2);std::string s="observation_date,DGS10\n";for(unsigned y=2020;y<2026;++y)for(unsigned m=1;m<=12;++m){char row[64];snprintf(row,sizeof(row),"%u-%02u-01,4.2\n",y,m);s+=row;}chunks(csv,s,13);assert(csv.finish());MacroView v;v.count=1;v.rows[0]=csv.row();csv.chart(v,0);assert(v.valid()&&v.points==60&&v.from==20210101&&v.zoomFrom==20250101&&v.to==20251201);for(auto p:v.chart)assert(p==500);}
    assert(!speaksToo(App::Macro)&&needsInternet(App::Macro)&&!needsComputer(App::Macro));
    assert(!strstr(keyHint(App::Macro,SettingsPage::List,false,false,false,false),"添加"));
    assert(strstr(keyHint(App::Macro,SettingsPage::List,false,false,false,false),"换页"));
    assert(strstr(keyHint(App::Macro,SettingsPage::List,false,false,false,false,true),"12/60"));
    for(unsigned tab=0;tab<5;++tab)assert(!strstr(keyHint(App::Macro,SettingsPage::List,false,false,false,false,false,tab),"删除"));
    for(unsigned i=0;i<kFunctions.size();++i){assert(kMenuStarts[menuPageOf(i)+1]-kMenuStarts[menuPageOf(i)]<=9);for(unsigned j=0;j<i;++j)assert(kFunctions[i]!=kFunctions[j]);}
    static_assert(unsigned(App::Walkie)==46&&unsigned(App::Macro)==47); // Keep the published main branch app IDs.
    static_assert(kFunctions.size()==43);
}
'''
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="passport-macro-") as directory:
            cpp = Path(directory) / "macro_test.cpp"
            executable = Path(directory) / "macro_test"
            cpp.write_text(source)
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            "-I", str(ROOT / "firmware/passport_suite/main"),
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
