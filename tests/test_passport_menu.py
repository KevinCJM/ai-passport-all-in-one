"""Exercise the firmware layout and its shared browser wire format, without an ESP32."""
import base64
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys
import threading
import urllib.request
import importlib.util
from types import SimpleNamespace
from unittest.mock import Mock,patch

ROOT = Path(__file__).resolve().parents[1]


class MenuTests(unittest.TestCase):
    def test_assistant_menu_routes_share_device_transport_and_origin_guard(self):
        sys.path.insert(0,str(ROOT/'scripts'))
        # Route tests need no host audio/cloud services and never instantiate a physical Device.
        spec=importlib.util.spec_from_file_location('menu_device_under_test',ROOT/'scripts/passport_device.py')
        device=importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules,{'passport_services':SimpleNamespace(Services=object),'passport_device_cloud':SimpleNamespace(DeviceXiaoZhi=object)}):
            spec.loader.exec_module(device)
        for transport in ('usb','ble'):
            paired=SimpleNamespace(config={'ui_port':0},transport=transport,request=Mock(return_value={'revision':123}),menu=Mock(return_value={'queued':True,'ticket':7}))
            server=device.ThreadingHTTPServer(('127.0.0.1',0),device.Handler);paired.config['ui_port']=server.server_port
            thread=threading.Thread(target=server.serve_forever,daemon=True)
            with patch.object(device,'DEVICE',paired,create=True),patch.object(device,'UI_PORT',server.server_port):
                thread.start();base='http://127.0.0.1:'+str(server.server_port)
                try:
                    with urllib.request.urlopen(base+'/api/menu') as response:self.assertEqual(json.load(response),{'revision':123})
                    paired.request.assert_called_with('/menu')
                    with urllib.request.urlopen(base+'/api/menu/catalog?group=4') as response:self.assertEqual(response.status,200)
                    paired.request.assert_called_with('/menu/catalog?group=4')
                    data={'layout':'test','revision':123};request=urllib.request.Request(base+'/api/menu',data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
                    with urllib.request.urlopen(request) as response:self.assertEqual(json.load(response)['ticket'],7)
                    paired.menu.assert_called_once_with(data)
                    with urllib.request.urlopen(base+'/menu-editor.js') as response:self.assertIn(b'PassportMenu',response.read())
                    request=urllib.request.Request(base+'/api/menu',data=b'{}',headers={'Content-Type':'application/json','Origin':'https://example.org'})
                    with self.assertRaises(urllib.error.HTTPError) as denied:urllib.request.urlopen(request)
                    self.assertEqual(denied.exception.code,403)
                finally:server.shutdown();server.server_close();thread.join(2)

    def test_layout_boundaries_and_browser_roundtrip(self):
        source = r'''
#include "menu_layout.hpp"
#include <cassert>
#include <fstream>
#include <random>
using namespace passport;
int main(int argc,char** argv){
    assert(argc==2);auto d=defaultMenuLayout();assert(d.valid());
    std::ofstream file(argv[1],std::ios::binary);file.write(reinterpret_cast<char*>(&d),sizeof(d));file.close();
    MenuView view;view.build(d);assert(view.count==44&&view.pages==5);
    for(auto app:kFunctions)d.show(app,app==App::Settings);
    view.build(d);assert(d.valid()&&view.count==1&&view.pages==1&&kFunctions[view.items[0]]==App::Settings);
    auto bad=d;bad.show(App::Settings,false);assert(!bad.valid());bad=d;bad.order[0][0]=0;assert(!bad.valid());
    bad=d;bad.mode=4;assert(!bad.valid());bad=d;bad.categories[1]=0;assert(!bad.valid());
    bad=d;bad.names[0][0]=char(0xff);assert(!bad.valid());bad=d;memset(bad.names[0],'a',25);assert(!bad.valid());
    bad=defaultMenuLayout();memset(bad.assignment,0,sizeof(bad.assignment));assert(!bad.valid());
    bad=defaultMenuLayout();bad.pageCount=9;assert(!bad.valid());bad=d;bad.assignment[10]=8;assert(!bad.valid());
    // A new firmware app is appended and shown when an older catalog record is loaded.
    auto old=defaultMenuLayout();for(auto& list:old.order){unsigned n=0;for(unsigned i=0;i<old.catalogCount;++i)if(list[i]!=unsigned(App::Macro))list[n++]=list[i];list[n]=255;}--old.catalogCount;
    assert(!old.valid()&&old.valid(true)&&old.upgrade()&&old.valid()&&old.shown(App::Macro));
    std::mt19937 random(42);
    for(unsigned trial=0;trial<1000;++trial){
        d=defaultMenuLayout();d.mode=trial%4;
        for(auto& list:d.order)std::shuffle(list,list+d.catalogCount,random);
        std::shuffle(d.categories,d.categories+5,random);std::shuffle(d.pages,d.pages+5,random);
        unsigned expected=0;for(auto app:kFunctions){const bool show=app==App::Settings||random()%2;d.show(app,show);expected+=show;}
        assert(d.valid());view.build(d);assert(view.count==expected&&view.pages>=1&&view.pages<=8);
        uint64_t seen=0;for(unsigned i=0;i<view.count;++i){const auto app=kFunctions[view.items[i]];assert(d.shown(app));const auto bit=uint64_t{1}<<unsigned(app);assert(!(seen&bit));seen|=bit;assert(view.position(view.items[i])==i);assert(view.page(i)<view.pages);}
        for(unsigned page=0;page<view.pages;++page){assert(view.starts[page]<view.starts[page+1]);if(d.mode!=MenuLayout::List)assert(view.starts[page+1]-view.starts[page]<=9);else assert(view.pages==1);}
    }
}
'''
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder/'test.cpp').write_text(source)
            subprocess.run([shutil.which('clang++') or 'c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I'+str(ROOT/'firmware/passport_suite/main'),
                            str(folder/'test.cpp'), '-o', str(folder/'test')], check=True)
            subprocess.run([str(folder/'test'), str(folder/'default.bin')], check=True)
            raw = (folder/'default.bin').read_bytes()
            self.assertEqual(len(raw),481)
            node = r'''
const fs=require('fs'),vm=require('vm');vm.runInThisContext(fs.readFileSync(process.argv[1],'utf8'));
const input=process.argv[2],decoded=PassportMenu.decode(input);
if(decoded.orders[0].length!==44||decoded.names[0]!=='小智语音')throw Error('wrong layout');
if(PassportMenu.encode(decoded)!==input)throw Error('wire roundtrip mismatch');
decoded.mode=2;decoded.names[0]='我的常用';decoded.orders[2].reverse();
if(PassportMenu.decode(PassportMenu.encode(decoded)).names[0]!=='我的常用')throw Error('UTF8 mismatch');
decoded.names[0]='a'.repeat(9);let rejected=false;try{PassportMenu.encode(decoded)}catch(e){rejected=true}if(!rejected)throw Error('name bound');
console.log(JSON.stringify({bytes:481,apps:44,random_layouts:1000,wire_roundtrip:true}));
'''
            subprocess.run(['node','-e',node,str(ROOT/'scripts/passport_menu.js'),base64.b64encode(raw).decode()],check=True)


if __name__ == '__main__':
    unittest.main()
