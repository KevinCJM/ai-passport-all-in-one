// 高铁查询 host check: c++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/passport_suite/main scripts/test_train.cpp -o /tmp/test_train && /tmp/test_train
#include "train.hpp"
#include <cassert>
using namespace passport;
int main(){
  setenv("TZ","CST-8",1);tzset();
  const time_t now=train::epoch(20261108,14*60+20);
  const char* cases[][3]={{"G7","G7","20261108"},{"g 7。","G7","20261108"},{"明天的D701","D701","20261109"},{"鸡七","G7","20261108"},{"高铁七","G7","20261108"},
   {"K五九九 11月9号","K599","20261109"},{"动车七零一","D701","20261108"},{"G一千二百八十一次","G1281","20261108"},{"查一下G1281后天","G1281","20261110"},
   {"1461次","1461","20261108"},{"Z三五","Z35","20261108"},{"C2001","C2001","20261108"},{"高铁G7","G7","20261108"},{"T七","T7","20261108"},{"G十二","G12","20261108"}};
  for(auto& c:cases){auto r=train::parse(c[0],now);char d[16];snprintf(d,sizeof d,"%u",r.date);printf("%s -> %s %s %s\n",c[0],r.ok?r.code:"-",d,r.error?r.error:"");assert(r.ok&&!strcmp(r.code,c[1])&&!strcmp(d,c[2]));}
  assert(!train::parse("你好",now).ok);assert(!train::parse("11月9号",now).ok);
  assert(train::late("预计G19次列车，济南西站到达时间为15:32",15*60+23)==9);assert(train::late("目前暂无",600)==kTrainNoLate);assert(train::late("预计时间为00:05",1439)==6);
  assert(train::minutes("0117",1)==1440+77&&train::minutes("--",0)==kTrainNoTime);
  TrainTrips t;TrainTrip a{"G7",20261108,0,2,"北京南","南京南",540,746,""};TrainTrip b{"D701",20261109,1,3,"北京西","石家庄",600,700,""};
  assert(t.put(b)==0&&t.put(a)==0&&t.count==2&&!strcmp(t.items[1].code,"D701")&&t.valid());
  assert(t.today(now)==-1);assert(t.today(train::epoch(20261108,500))==0);
  assert(t.prune(train::epoch(20261109,700+121))&&t.count==0);
  puts("TRAIN_OK");
}

