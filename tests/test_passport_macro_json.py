"""Public upstream fixtures exercise the actual bounded ESP32 JSON reader."""
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
FIXTURES=ROOT/'tests/fixtures/macro'
class MacroJsonTests(unittest.TestCase):
 def test_sources_chunking_units_and_invalid_json(self):
  source=r'''
#include "macro_json.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
using namespace passport;
int main(int argc,char** argv){
 static_assert(sizeof(MacroJson)<2048&&sizeof(MacroPoints)<512&&sizeof(MacroView)<512);
 assert(argc==3);unsigned id=atoi(argv[1]);std::ifstream file(argv[2]);std::string body((std::istreambuf_iterator<char>(file)),{});assert(!body.empty());
 for(unsigned chunk=1;chunk<32;++chunk){MacroJson parser(id);
  for(size_t i=0;i<body.size();i+=chunk)parser.feed(body.data()+i,std::min<size_t>(chunk,body.size()-i));
  assert(parser.finish());auto row=parser.points.row(id);assert(row.date&&std::isfinite(row.value));
  MacroView view;view.count=1;view.group=kMacroSeries[id].group;view.rows[0]=row;parser.points.chart(view,0);assert(view.valid()&&view.points<=60);
  if(chunk==1)std::cout<<row.date<<" "<<row.value<<" "<<row.previous<<" "<<unsigned(view.points)<<"\n";
 }
 for(auto bad:{body.substr(0,body.size()-4),body+"junk",std::string("<html>unavailable</html>"),std::string("{\"success\":true,\"data\":[{\"REPORT_DATE\":\"2026-10-01\",\"SUM_SAME\":9}]}"),std::string("{\"success\":true,\"result\":{\"data\":[{\"REPORT_DATE\":\"2026-02-30\",\"SUM_SAME\":9}]}}")}){
  MacroJson p(id);p.feed(bad.data(),bad.size());assert(!p.finish());
 }
 if(id==14){
  for(const char* v:{"null","\"--\""}){std::string missing="{\"success\":true,\"result\":{\"data\":[{\"REPORT_DATE\":\"2026-10-01\",\"SUM_SAME\":"+std::string(v)+"}]}}";MacroJson p(id);p.feed(missing.data(),missing.size());assert(!p.finish());}
  std::string zero="{\"success\":true,\"result\":{\"data\":[{\"SUM_SAME\":0,\"REPORT_DATE\":\"2026-10-01\"}]}}";MacroJson p(id);p.feed(zero.data(),zero.size());assert(p.finish()&&p.points.row(id).value==0);
 }
 if(id==22||id==23||id==24){auto bad=body;auto at=bad.find(id==22?"\\u4e07\\u6876/\\u5929":"\\u5428");assert(at!=std::string::npos);bad.replace(at,id==22?19:6,"bad-unit");MacroJson p(id);p.feed(bad.data(),bad.size());assert(!p.finish());}
 MacroPoints points;assert(points.add(20261001,1)&&points.add(20261003,3)&&points.add(20261002,2));assert(points.row(id).value==3&&points.row(id).previous==2);assert(!points.add(20261003,4));
}
'''
  compiler=shutil.which('clang++') or shutil.which('g++')
  with tempfile.TemporaryDirectory(prefix='passport-macro-json-') as tmp:
   cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(source)
   subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(ROOT/'firmware/passport_suite/main'),str(cpp),'-o',str(binary)],check=True)
   fields={14:'SUM_SAME',15:'NATIONAL_SAME',16:'BASE_SAME',17:'MAKE_INDEX',18:'BASIC_CURRENCY_SAME',19:'LPR1Y'}
   for path in sorted(FIXTURES.glob('*.json')):
    with self.subTest(series=path.stem):
     i=int(path.stem);d=json.loads(path.read_text())
     if i==22:rows=[(date,v['美国国内原油总量'][0]) for date,v in d['values'].items()]
     elif i in (23,24):rows=[r[:2] for r in d['data']['values']]
     else:rows=[(r['TRADE_DATE' if i==19 else 'REPORT_DATE'][:10],r[fields.get(i,'INDICATOR_VALUE')]) for r in d['result']['data']]
     rows=sorted((date,float(value)) for date,value in rows if value is not None and math.isfinite(float(value)))
     result=subprocess.check_output([str(binary),str(i),str(path)],text=True).split()
     self.assertEqual(int(result[0]),int(rows[-1][0].replace('-','')))
     self.assertAlmostEqual(float(result[1]),rows[-1][1],delta=max(.001,abs(rows[-1][1])*1e-5))
     self.assertAlmostEqual(float(result[2]),rows[-2][1],delta=max(.001,abs(rows[-2][1])*1e-5))
     self.assertEqual(int(result[3]),min(60,len(rows)))
if __name__=='__main__':unittest.main()
