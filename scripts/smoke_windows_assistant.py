#!/usr/bin/env python3
"""Windows smoke test on a real desktop (CI: after build_passport_assistant_windows.py). Everything but the badge:

- the packaged exe starts, serves the page and the 电脑助手 API, refuses a second copy, pauses/resumes, and
  sets start-with-Windows in the registry;
- 语音输入's paste, Enter and delete-line, through the same passport_windows / passport_dictation code, typed into
  Notepad and read back; the clipboard is put back afterwards;
- Start-menu programs are found and opened; idle time and keep-awake calls work.
A screenshot (tray + page) and a JSON report go to the folder given as the first argument.
"""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
APP=ROOT/'experiments/passport-assistant-build/windows/dist/AI Passport Assistant/AI Passport Assistant.exe'
WEB='http://127.0.0.1:8768'
report={};failures=[]


def check(name,ok,detail=None):
    report[name]={'ok':bool(ok),'detail':detail};print(('PASS ' if ok else 'FAIL ')+name,'' if detail is None else detail,flush=True)
    if not ok:failures.append(name)


def until(test,seconds=20):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try:
            value=test()
            if value:return value
        except Exception:pass
        time.sleep(.3)
    return None


def get(path):
    with urllib.request.urlopen(WEB+path,timeout=5) as response:return json.load(response) if path.startswith('/api') else response.read().decode()


def post(path,body):
    request=urllib.request.Request(WEB+path,data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=10) as response:return json.load(response)


def notepad_text(edit):
    user32=ctypes.windll.user32;size=user32.SendMessageW(edit,0x000E,0,0)  # WM_GETTEXTLENGTH
    buffer=ctypes.create_unicode_buffer(size+1);user32.SendMessageW(edit,0x000D,size+1,buffer);return buffer.value  # WM_GETTEXT


def foreground(window):
    """Windows only lets the foreground owner hand focus on; a synthetic Alt press counts as input."""
    import passport_windows as win
    user32=ctypes.windll.user32
    for _ in range(10):
        win.press([(0x12,)]);user32.ShowWindow(window,9);user32.SetForegroundWindow(window)
        if user32.GetForegroundWindow()==window:return True
        time.sleep(.3)
    return False


def typing():
    import passport_windows as win
    import passport_dictation
    user32=ctypes.windll.user32
    user32.FindWindowW.restype=user32.FindWindowExW.restype=user32.GetForegroundWindow.restype=ctypes.c_void_p
    user32.FindWindowExW.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_wchar_p]
    user32.SetForegroundWindow.argtypes=[ctypes.c_void_p];user32.ShowWindow.argtypes=[ctypes.c_void_p,ctypes.c_int]
    user32.SendMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_void_p]
    notepad=subprocess.Popen(['notepad.exe'])
    try:
        window=until(lambda:user32.FindWindowW('Notepad',None))
        edit=window and user32.FindWindowExW(window,None,'Edit',None)
        check('notepad_ready',bool(edit))
        if not edit:return
        check('notepad_foreground',foreground(window))
        check('notepad_not_a_terminal',not win.foreground_terminal())
        win.set_text('剪贴板原来的内容')
        restore=passport_dictation.paste('第一行：语音输入粘贴测试',win.keys);restore()
        check('paste_text',until(lambda:'第一行：语音输入粘贴测试' in notepad_text(edit),5),notepad_text(edit))
        check('clipboard_restored',win.get_text()=='剪贴板原来的内容',win.get_text())
        win.keys('enter');restore=passport_dictation.paste('第二行要删掉',win.keys);restore()
        check('enter_then_paste',until(lambda:notepad_text(edit)=='第一行：语音输入粘贴测试\r\n第二行要删掉',5),notepad_text(edit))
        win.keys('delete_line');time.sleep(.5)
        check('delete_line',notepad_text(edit)=='第一行：语音输入粘贴测试\r\n',notepad_text(edit))
    finally:notepad.kill()


def system():
    import passport_windows as win
    import passport_mac_open as opener
    idle=win.idle_seconds();check('idle_seconds',0<=idle<3600,round(idle,1))
    win.Awake().hold(True);win.Awake().hold(False);check('keep_awake',True)
    check('screen_not_locked',not win.screen_locked())
    apps=opener.start_apps();check('start_apps_listed',len(apps)>5,len(apps))
    found=opener.find('记事本',apps) or opener.find('notepad',apps);check('start_apps_find_notepad',found,found and found[1])
    if found:
        before={p.pid for p in __import__('psutil').process_iter(['name']) if (p.info['name'] or '').lower()=='notepad.exe'}
        check('open_app',opener.open_app(found[0]))
        new=until(lambda:[p for p in __import__('psutil').process_iter(['name']) if (p.info['name'] or '').lower()=='notepad.exe' and p.pid not in before],10)
        check('open_app_started_notepad',new)
        for process in new or []:process.kill()


def packaged(out):
    import winreg
    data=tempfile.mkdtemp(prefix='passport-smoke-');env=dict(os.environ,PASSPORT_DATA_DIR=data)
    first=subprocess.Popen([str(APP),'--background'],env=env)
    try:
        state=until(lambda:get('/api/assistant'),40)
        check('web_assistant_api',state and state.get('platform')=='windows',state and state.get('status'))
        check('web_page','电脑助手' in get('/'))
        check('features_rows',state and [r[0] for r in state['features']]==['语音输入','Codex'],state and state.get('features'))
        check('voice_component',state and state['features'][0][1] is True)
        try:get('/api/state');offline=None
        except urllib.error.HTTPError as error:offline=error.code
        check('badge_apis_wait_for_badge',offline==503,offline)
        second=subprocess.run([str(APP)],env=env,timeout=30)
        check('single_instance',second.returncode==0 and first.poll() is None)
        post('/api/assistant',{'action':'toggle'});check('pause',until(lambda:get('/api/assistant')['status']=='助手已暂停',10))
        post('/api/assistant',{'action':'toggle'});check('resume',until(lambda:get('/api/assistant')['status']!='助手已暂停',10))
        post('/api/assistant',{'action':'login','on':True})
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
            value=winreg.QueryValueEx(key,'AI Passport Assistant')[0]
        check('autostart_on',value==f'"{APP}" --background' and get('/api/assistant')['login'],value)
        post('/api/assistant',{'action':'login','on':False});check('autostart_off',not get('/api/assistant')['login'])
        time.sleep(4)  # the second launch opened the page in the browser; let it draw
        from PIL import ImageGrab
        ImageGrab.grab(all_screens=True).save(out/'screenshot.png');check('screenshot',True)
        check('still_running',first.poll() is None)
    finally:
        subprocess.run(['taskkill','/F','/T','/PID',str(first.pid)],capture_output=True)


def main():
    out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
    for step in (packaged,lambda _:typing(),lambda _:system()):
        try:step(out)
        except Exception as error:check('step_error',False,f'{type(error).__name__}: {error}')
    (out/'smoke.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('SMOKE',len(report)-len(failures),'passed',len(failures),'failed',failures,flush=True)
    sys.exit(1 if failures else 0)


if __name__=='__main__':main()
