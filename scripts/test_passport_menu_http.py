#!/usr/bin/env python3
"""Real HTTP/NVS menu transactions in the C3 connectivity firmware. No physical device."""
import base64
import json
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'firmware/passport_connectivity/build'
OUT=ROOT/'releases/menu-customization-http-test'
EMU=Path.home()/'.local/share/esp32-agent/bin/esp-emu'
HTTP=urllib.request.build_opener(urllib.request.ProxyHandler({}))


def main():
    OUT.mkdir(parents=True,exist_ok=True);flash=OUT/'flash.bin';flash.write_bytes((BUILD/'merged_flash.bin').read_bytes())
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base='http://127.0.0.1:'+str(port)
    def api(path,data=None,expected=200):
        request=urllib.request.Request(base+path,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
        try:response=HTTP.open(request,timeout=3)
        except urllib.error.HTTPError as error:assert error.code==expected,(error.code,expected,error.read());return None
        assert response.status==expected
        with response:return json.load(response)
    def start(name):
        log=(OUT/name).open('wb')
        process=subprocess.Popen([str(EMU),'--chip','esp32c3','--firmware',str(flash),'--elf',str(BUILD/'passport_connectivity.elf'),'--wifi-auth','wpa2-psk','--net',f'user,hostfwd=tcp:127.0.0.1:{port}-:80','--log-color','never','--save-state','--exit-on','SIM_RESTART_SAFE','--timeout','90s'],stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT)
        until=time.monotonic()+45
        while time.monotonic()<until:
            if process.poll() is not None:raise RuntimeError('C3 exited: '+(OUT/name).read_text(errors='replace')[-3000:])
            try:return process,log,api('/menu')
            except (OSError,ValueError):time.sleep(.2)
        process.kill();raise TimeoutError('C3 HTTP did not start')
    def stop(process,log):
        if process.poll() is None:
            process.stdin.write(b'900 save_exit\n');process.stdin.flush()
            try:process.wait(timeout=12)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
        log.close()
    def save(state,raw):
        encoded=base64.b64encode(raw).decode();queued=api('/menu',{'layout':encoded,'revision':state['revision']});until=time.monotonic()+10
        while time.monotonic()<until:
            next_state=api('/menu')
            if next_state['done']==queued['ticket']:
                assert next_state['result']==0 and next_state['layout']==encoded
                return next_state
            time.sleep(.1)
        raise TimeoutError('menu save acknowledgement missing')
    process,log,state=start('http-serial.log')
    try:
        catalog=[api('/menu/catalog?group='+str(g)) for g in range(6)];ids=[a['id'] for g in catalog for a in g['apps']];assert len(ids)==len(set(ids))
        with HTTP.open(base+'/menu-editor.js') as response:assert response.read()==(ROOT/'scripts/passport_menu.js').read_bytes()
        api('/menu/catalog?group=8',expected=400)
        original=state;raw=bytearray(base64.b64decode(state['layout']));raw[1]=1;raw[82:125]=raw[82:125][::-1]
        state=save(state,raw);api('/menu',{'layout':state['layout'],'revision':original['revision']},expected=409)
        bad=bytearray(raw);bad[4+10//8]|=1<<(10%8);api('/menu',{'layout':base64.b64encode(bad).decode(),'revision':state['revision']},expected=400)
        assert api('/menu')['layout']==state['layout']
        raw[1]=2;raw[3]=2;raw[4:12]=bytes(8);keep={1,4,10,35,47}
        for identity in ids:
            if identity not in keep:raw[4+identity//8]|=1<<(identity%8)
        raw[210:274]=bytes(64)
        for identity in (10,35,47):raw[210+identity]=1
        raw[274:276]=bytes((1,0));raw[282:482]=bytes(200)
        for group,name in enumerate(('常用','财经工具')):encoded=name.encode();raw[282+group*25:282+group*25+len(encoded)]=encoded
        state=save(state,raw);saved=state['layout']
    finally:stop(process,log)
    process,log,cold=start('http-coldboot.log')
    try:assert cold['layout']==saved
    finally:stop(process,log)
    report={'status':'PASS','apps':43,'continuous_order_saved':True,'custom_layout_saved':True,'invalid_layout_rejected':True,'stale_revision_rejected':True,'cold_boot_persisted':True,'physical_device_accessed':False}
    (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)


if __name__=='__main__':main()
