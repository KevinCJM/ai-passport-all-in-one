#!/usr/bin/env python3
"""Run this repository's C3 firmware in the installed ESP32 emulator and capture LVGL pixels."""
import base64
import hashlib
import json
from pathlib import Path
import re
import selectors
import struct
import subprocess
import time
import zlib

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'releases/menu-customization-test'
BUILD=ROOT/'firmware/passport_suite/build'
EMU=Path.home()/'.local/share/esp32-agent/bin/esp-emu'


def png(path,pixels,width=240,height=320):
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))


def identity_fixture():
    def color(r,g,b):return struct.pack('<H',(r>>3)<<11|(g>>2)<<5|(b>>3))
    blob=bytearray(b'\xff'*(0x10000+240*320*2));avatar=[]
    for y in range(156):
        for x in range(96):
            head=(x-48)**2+(y-48)**2<900;body=y>84 and abs(x-48)<20+(y-84)//3
            avatar.append((246,214,180,255) if head else (58,120,210,255) if body else (0,0,0,0))
    blob[0x1000:0x1000+96*156*2]=b''.join(color(*p[:3]) for p in avatar)
    blob[0x1000+96*156*2:0x1000+96*156*3]=bytes(p[3] for p in avatar)
    blob[0x10000:]=b''.join(color(30+x//3,60+y//4,150+(x+y)%90) for y in range(320) for x in range(240))
    blob[16:304]=struct.pack('<64s64s160s',b'Kevin',b'Developer','模拟器测试'.encode())
    blob[:16]=struct.pack('<4sIII',b'PPID',1,zlib.crc32(blob[16:]),3)
    return blob


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    original=(BUILD/'merged_flash.bin').read_bytes();digest=hashlib.sha256(original).hexdigest()
    image=bytearray(original);image.extend(b'\xff'*(0x800000-len(image)));fixture=identity_fixture();image[0x6a0000:0x6a0000+len(fixture)]=fixture
    flash=OUT/'simulation-flash.bin';flash.write_bytes(image)
    command=[str(EMU),'--chip','esp32c3','--firmware',str(flash),'--wifi-auth','wpa2-psk','--net','user','--log-color','never','--save-state','--exit-on','SUITE_DONE','--timeout','590s']
    started=time.monotonic();proc=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    frames=[];cases=[];memory=[];active=None;pending=b'';failures=[]
    colors=[bytes((((p>>11)&31)*255//31,((p>>5)&63)*255//63,(p&31)*255//31)) for p in range(65536)]
    with (OUT/'serial.log').open('wb') as log,selectors.DefaultSelector() as selector:
        selector.register(proc.stdout,selectors.EVENT_READ)
        try:
            while selector.get_map():
                if time.monotonic()-started>600:raise TimeoutError('C3 suite timed out')
                for key,_ in selector.select(.25):
                    raw=key.fileobj.read1(65536)
                    if not raw:selector.unregister(key.fileobj);continue
                    log.write(raw);log.flush();pending+=raw
                    while b'\n' in pending:
                        line,pending=pending.split(b'\n',1);line=line.decode(errors='replace').strip()
                        if any(marker in line for marker in ('SUITE_FAIL','Guru Meditation Error','assert failed','CORRUPT HEAP','abort() was called')):failures.append(line);proc.terminate()
                        if line.startswith(('SIM_BOOT_OK','CASE_PASS','MENU_','STRESS','SUITE_PASS','NETWORK_OK')):print(line,flush=True)
                        if line.startswith('CASE_PASS '):cases.append(line[10:])
                        if line.startswith('MEM '):memory.append(json.loads(line[4:]))
                        if line.startswith('FRAME_BEGIN '):
                            _,name,w,h=line.split();assert (w,h)==('240','320') and active is None
                            active=(name,bytearray(240*320*3),bytearray(240*320))
                        elif line.startswith('ROW ') and active:
                            _,x,y,data=line.split();x,y=int(x),int(y);raw=base64.b64decode(data,validate=True);length=len(raw)//2
                            assert len(raw)%2==0 and 0<=y<320 and 0<=x and x+length<=240
                            offset=y*240+x;active[1][offset*3:(offset+length)*3]=b''.join(colors[p[0]] for p in struct.iter_unpack('<H',raw));active[2][offset:offset+length]=b'\1'*length
                        elif line.startswith('FRAME_END '):
                            assert active and active[0]==line.split()[1] and all(active[2]),'incomplete LVGL frame'
                            png(OUT/(active[0]+'.png'),active[1]);frames.append(active[0]);active=None
            proc.wait(timeout=5)
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
    log=(OUT/'serial.log').read_text(errors='replace')
    assert proc.returncode==0 and not failures,failures or log[-4000:]
    for marker in ('SIM_BOOT_OK suite','NETWORK_OK real_wifi_driver=1','SUITE_PASS','SUITE_DONE','menu_layout_categories_continuous_custom_order_hidden_voice_nvs_and_recovery'):assert marker in log,marker
    for frame in ('menu_continuous_top','menu_continuous_scroll','menu_custom_finance','menu_custom_daily','menu_settings_only'):assert frame in frames,frame
    cold=subprocess.run([str(EMU),'--chip','esp32c3','--firmware',str(flash),'--wifi-auth','wpa2-psk','--net','user','--log-color','never','--exit-on','MENU_BOOT','--timeout','30s'],capture_output=True,timeout=40)
    coldlog=cold.stdout.decode(errors='replace');(OUT/'cold-boot.log').write_text(coldlog)
    assert cold.returncode==0 and 'MENU_BOOT mode=2 visible=6 pages=2' in coldlog,coldlog[-4000:]
    assert hashlib.sha256((BUILD/'merged_flash.bin').read_bytes()).hexdigest()==digest
    counts=re.search(r'SUITE_PASS checks=(\d+) switches=(\d+) rejected_events=(\d+) queue_drops=(\d+)',log);assert counts
    report={'status':'PASS','chip':'esp32c3','checks':int(counts[1]),'switches':int(counts[2]),'frames':frames,'cases':cases,'memory':memory,'menu_cold_boot_persisted':True,'physical_device_accessed':False,'physical_device_flashed':False,'image_sha256':digest,'app_bytes':(BUILD/'passport_suite.bin').stat().st_size,'elapsed_seconds':round(time.monotonic()-started,1)}
    (OUT/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    cards=''.join('<figure><img width="240" height="320" src="'+name+'.png"><figcaption>'+name+'</figcaption></figure>' for name in frames if name.startswith('menu_'))
    (OUT/'gallery.html').write_text('<!doctype html><meta charset="utf-8"><title>菜单自定义 · C3 仿真</title><style>body{background:#10191f;color:#e4ecee;font:16px system-ui}main{display:flex;flex-wrap:wrap;gap:20px}figure{margin:10px}img{border-radius:12px}</style><h1>菜单自定义 · C3 固件实际像素</h1><main>'+cards+'</main>')
    print(json.dumps({key:report[key] for key in ('status','checks','switches','menu_cold_boot_persisted','app_bytes','elapsed_seconds')},ensure_ascii=False),flush=True)


if __name__=='__main__':main()
