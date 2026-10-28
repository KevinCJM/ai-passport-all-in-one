"""语音输入: the device's microphone over BLE or USB, XiaoZhi's ASR from this Mac, the text pasted at the cursor.

No Mac microphone, IME or audio driver is involved, and nothing is kept but the text. Like the device's 语音笔记,
the socket closes as soon as the text arrives, so XiaoZhi's spoken reply is never received.
"""
import ctypes
import json
import queue
import select
import struct
import subprocess
import threading
import time
from passport_paths import helper

FRAME=1920      # 60 ms of 16 kHz PCM16: one Opus frame
STOP=object()   # queued after the last frame: the utterance is complete
HEAD=150        # bytes of the typed text shown on the device (its lines hold 160)
_opus=None


def opus():
    global _opus
    if _opus is None:
        bundled=helper('libopus.0.dylib')
        lib=ctypes.CDLL(str(bundled) if bundled.is_file() else '/opt/homebrew/lib/libopus.0.dylib')
        lib.opus_encoder_create.restype=ctypes.c_void_p
        lib.opus_encoder_create.argtypes=[ctypes.c_int32,ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_int)]
        lib.opus_encode.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_int32]
        lib.opus_encoder_destroy.argtypes=[ctypes.c_void_p]
        _opus=lib
    return _opus


class Encoder:
    def __init__(self):
        error=ctypes.c_int();self.state=opus().opus_encoder_create(16000,1,2048,ctypes.byref(error))  # 2048: VOIP
        if error.value or not self.state:raise RuntimeError('Opus encoder unavailable')
        self.buffer=ctypes.create_string_buffer(1500)

    def encode(self,pcm):
        size=opus().opus_encode(self.state,pcm,FRAME//2,self.buffer,len(self.buffer))
        if size<0:raise RuntimeError('Opus encoding failed')
        return self.buffer.raw[:size]

    def __del__(self):
        if getattr(self,'state',None):opus().opus_encoder_destroy(self.state);self.state=None


def xiaozhi():
    """The transcription identity's XiaoZhi socket after the hello, and its session id."""
    from passport_device_cloud import HELLO,device_socket,transcriber
    socket=device_socket(transcriber(),open_timeout=8)
    try:
        socket.send(json.dumps(HELLO));until=time.monotonic()+8
        while True:
            message=socket.recv(timeout=max(.1,until-time.monotonic()))
            if isinstance(message,str) and json.loads(message).get('type')=='hello':return socket,json.loads(message).get('session_id','')
    except Exception:
        socket.close();raise


class Recognizer:
    """One utterance through XiaoZhi in manual listen mode. It connects while the user speaks: frames wait in the
    queue meanwhile, so nothing said in the first second is lost. text is the result ('' when nothing was heard)."""
    def __init__(self,connect=xiaozhi):
        self.connect=connect;self.encoder=Encoder();self.pcm=b'';self.outgoing=queue.Queue()
        self.text=None;self.error=None;self.cancelled=self.finished=False;self.socket=None;self.done=threading.Event()
        threading.Thread(target=self.run,daemon=True).start()

    def audio(self,pcm):
        self.pcm+=pcm
        while len(self.pcm)>=FRAME:self.outgoing.put(self.encoder.encode(self.pcm[:FRAME]));self.pcm=self.pcm[FRAME:]

    def finish(self):
        if self.pcm:self.outgoing.put(self.encoder.encode(self.pcm.ljust(FRAME,b'\0')));self.pcm=b''
        self.outgoing.put(STOP);self.finished=True

    def cancel(self):
        self.cancelled=True;self.outgoing.put(None)
        if self.socket:self.socket.close()

    def run(self):
        try:
            self.socket,session=self.connect()
            if self.cancelled:return
            self.socket.send(json.dumps({'session_id':session,'type':'listen','state':'start','mode':'manual'}))
            while (item:=self.outgoing.get()) is not STOP:
                if item is None:return
                self.socket.send(item)
            self.socket.send(json.dumps({'session_id':session,'type':'listen','state':'stop'}))
            until=time.monotonic()+10
            while time.monotonic()<until:
                message=self.socket.recv(timeout=max(.1,until-time.monotonic()))
                if isinstance(message,str) and json.loads(message).get('type')=='stt':
                    self.text=json.loads(message).get('text') or '';return
            raise TimeoutError('XiaoZhi gave no text')
        except Exception as error:
            if not self.cancelled:self.error=error
        finally:
            if self.socket:self.socket.close()
            self.done.set()


def paste(text,keys):
    """Pastes text at the cursor. Returns what puts back the clipboard's former content (unless something else has
    changed it since); it waits for the app to have read the text first."""
    from AppKit import NSPasteboard,NSPasteboardItem,NSPasteboardTypeString
    board=NSPasteboard.generalPasteboard()
    saved=[{kind:item.dataForType_(kind) for kind in item.types()} for item in board.pasteboardItems() or ()]
    board.clearContents();board.setString_forType_(text,NSPasteboardTypeString)
    board.setString_forType_('','org.nspasteboard.TransientType')  # clipboard managers leave it out of their history
    mark=board.changeCount();keys('paste')
    def restore():
        # ponytail: fixed wait for the app to read the clipboard; a slower app would paste what is restored.
        time.sleep(1)
        if board.changeCount()!=mark:return
        board.clearContents();items=[]
        for kinds in saved:
            item=NSPasteboardItem.alloc().init()
            for kind,data in kinds.items():
                if data is not None:item.setData_forType_(data,kind)
            items.append(item)
        if items:board.writeObjects_(items)
    return restore


# Frontmost apps whose input line is a shell's: deleting it is Ctrl-E then Ctrl-U there.
TERMINALS={'com.apple.Terminal','com.googlecode.iterm2','dev.warp.Warp-Stable','com.mitchellh.ghostty','net.kovidgoyal.kitty',
           'org.alacritty','io.alacritty','com.github.wez.wezterm','co.zeit.hyper','org.tabby'}


class BluetoothDictation:
    def __init__(self,services):
        self.services=services; self.stop=threading.Event(); self.thread=None; self.context=None
        self.session=None; self.expected=None; self.recognizer=None; self.delivery=None; self.keyboard=None
        self.last=None   # the last text typed and when (epoch seconds)
        self.pressing=threading.Lock()
        self.stats={'state':'未连接','transport':'BLE','pcm_bytes':0,'sequence_errors':0,'sessions':0,'completed':0,'physical_adapter':False}

    def rpc(self,action,**data):
        reply=self.services.sim.ble(action,**data)
        if not reply.get('ok'): raise RuntimeError(reply.get('error','BLE operation failed'))
        return reply['result']

    def open(self,context):
        self.close(); self.context=context; self.stop=threading.Event()
        self.stats.pop('error',None)
        self.services.require_dictation_permissions()
        wired=getattr(self.services.sim,'transport',None)=='usb'
        self.stats.update(transport='USB' if wired else 'BLE',physical_adapter=not wired)
        if not wired and not self.services.sim.snapshot().get('ble_stack'): self.services.sim.command('ble_on')
        if not self.rpc('status')['connected']:
            self.rpc('connect'); self.rpc('pair')
        self.rpc('voice_subscribe')
        self.stats['state']='USB 就绪' if wired else '蓝牙就绪'
        self.thread=threading.Thread(target=self.run,daemon=True); self.thread.start()

    def ack(self,kind):
        self.rpc('voice_ack',kind=kind,generation=self.context[1],session=self.session)

    def status(self,value='ready',state=None):
        """The page's report: value ready, rec, asr, done, empty or fail; the last text, its size and time; the link."""
        text,at=self.last or ('',0)
        size='%d 字 · %s'%(len(text),time.strftime('%H:%M',time.localtime(at))) if text else ''
        state=state or self.stats['state'];self.stats['state']=state
        return {'state':state,'value':value,'lines':[text.encode()[:HEAD].decode('utf-8','ignore'),size,'USB' if self.stats['transport']=='USB' else '蓝牙','']}

    def publish(self,value,state=None):
        self.services.push(self.status(value,state),self.context)

    def key(self,command):
        if self.keyboard is None:
            self.keyboard=subprocess.Popen([str(helper('passport-dictation')),'keyboard_daemon'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,text=True,bufsize=1)
        self.keyboard.stdin.write(command+'\n'); self.keyboard.stdin.flush()
        if not select.select([self.keyboard.stdout],[],[],3)[0]:
            self.keyboard.terminate(); self.keyboard=None
            raise TimeoutError('Mac keyboard event acknowledgement missing')
        reply=json.loads(self.keyboard.stdout.readline() or '{}')
        if not reply.get('ok'):
            self.keyboard=None
            if reply.get('reason')=='post_event_access':raise PermissionError('Mac keyboard event permission required')
            raise RuntimeError('Mac keyboard event unavailable')

    def press(self,which):
        """语音输入's shortcuts, after any phrase still being typed: 1 Return, 2 delete the line the cursor is on."""
        with self.pressing:
            if self.delivery:self.delivery.join(30)
            try:
                if which==1:self.key('key 36 none');label='已回车'
                else:
                    from AppKit import NSWorkspace
                    app=NSWorkspace.sharedWorkspace().frontmostApplication()
                    if app is not None and app.bundleIdentifier() in TERMINALS:
                        # The shell's own line editing: to the end of the line, then delete back to its start.
                        self.key('key 14 ctrl');self.key('key 32 ctrl')
                    else:
                        # Text fields and editors: Command-Right to the end of the line, Command-Backspace to its start.
                        self.key('key 124 cmd');self.key('key 51 cmd')
                    label='已删除整行'
            except PermissionError:return self.publish('fail','需要辅助功能授权')
            except Exception as error:
                self.stats['error']=type(error).__name__;return self.publish('fail','快捷键失败，请重试')
            self.stats['keys']=self.stats.get('keys',0)+1;self.publish('key',label)

    def deliver(self,recognizer):
        """Waits for the utterance's text and types it; the device shows the outcome."""
        recognizer.done.wait(25)
        if recognizer.cancelled:return
        if recognizer.text is None:
            self.stats['error']=type(recognizer.error).__name__ if recognizer.error else 'TimeoutError'
            return self.publish('fail','识别失败，请重试')
        text=recognizer.text.strip()
        if not text:return self.publish('empty','没有听清，请再说一次')
        try:restore=paste(text,self.key)
        except PermissionError:return self.publish('fail','需要辅助功能授权')
        except Exception as error:
            self.stats['error']=type(error).__name__;return self.publish('fail','输入失败，请重试')
        self.last=(text,time.time());self.stats['completed']+=1;self.publish('done','已输入');restore()

    def handle(self,event):
        raw=bytes.fromhex(event['hex'])
        if event['report']==0: raise RuntimeError('BLE disconnected')
        if event['report']==1: return  # an older device's Doubao key taps: nothing to press now
        if event['report']!=2 or len(raw)!=176: raise ValueError('invalid C3 HID audio report')
        kind,generation,session,sequence,length=struct.unpack_from('<BIIHB',raw)
        if generation!=self.context[1]:
            self.stats['stale_reports']=self.stats.get('stale_reports',0)+1;return
        if length>160:raise ValueError('invalid C3 audio length')
        # A shortcut key between phrases (Down: Return, Up: delete the line); it belongs to no utterance.
        if kind==7:
            if length!=1 or raw[12] not in (1,2):raise ValueError('invalid C3 key report')
            threading.Thread(target=self.press,args=(raw[12],),daemon=True).start();return
        if kind==1:
            if self.session==session:return
            if self.session is not None:
                self.discard();self.stats['recovered_sessions']=self.stats.get('recovered_sessions',0)+1
            if raw[12:18]!=struct.pack('<IBB',16000,16,1): raise ValueError('unsupported C3 audio format')
            self.encoding=raw[18]
            if self.encoding not in (0,1):raise ValueError('unsupported C3 audio encoding')
            # The previous utterance still being typed goes first (the device waits 5 s for this ACK).
            if self.delivery:self.delivery.join(3)
            self.session=session; self.expected=sequence; self.recognizer=Recognizer()
            self.stats['sessions']+=1
            self.ack(1)
        if session!=self.session:
            self.stats['stale_reports']=self.stats.get('stale_reports',0)+1;return
        if sequence!=self.expected:
            self.stats['sequence_errors']+=1; raise ValueError('C3 audio sequence mismatch')
        self.expected=(sequence+1)&65535
        if kind==2:
            data=raw[12:12+length]
            if self.encoding:
                import audioop
                if len(data)<6:raise ValueError('short ADPCM block')
                predictor,index,count=struct.unpack_from('<hBH',data)
                if index>88 or not 1<=count<=310 or len(data)!=5+(count+1)//2:raise ValueError('invalid ADPCM block')
                data=audioop.adpcm2lin(data[5:],2,(predictor,index))[0][:count*2]
            elif length%2:raise ValueError('unaligned PCM')
            self.recognizer.audio(data);self.stats['pcm_bytes']+=len(data)
        elif kind==5:self.publish('rec','正在录音')
        elif kind==3:
            self.recognizer.finish();self.ack(2);self.publish('asr','正在识别')
            self.delivery=threading.Thread(target=self.deliver,args=(self.recognizer,),daemon=True);self.delivery.start()
        elif kind==4:self.session=None;self.expected=None
        elif kind==6:self.discard();self.publish('ready')

    def discard(self):
        """Drops an utterance still being recorded: nothing is typed. One already ended is still typed."""
        if self.recognizer and not self.recognizer.finished:self.recognizer.cancel()
        self.session=None;self.expected=None

    def run(self):
        try:
            while not self.stop.is_set() and self.services.requested_context==self.context:
                reply=self.rpc('voice_events')
                if not reply['connected']: raise RuntimeError('BLE disconnected')
                if reply['dropped']: raise RuntimeError('BLE host report queue dropped audio')
                for event in reply['events']: self.handle(event)
                self.stop.wait(.015)
        except Exception as error:
            self.stats['error']=str(error);self.discard()
            self.services.push(dict(self.status('fail','语音连接异常'),phase='error',lines=['确认：重新连接','','','']),self.context)

    def close(self):
        self.stop.set()
        if self.thread and self.thread is not threading.current_thread(): self.thread.join(5)
        if self.thread and self.thread.is_alive(): raise RuntimeError('BLE voice worker did not stop')
        self.thread=None;self.discard()
        if self.keyboard:
            self.keyboard.stdin.close()
            try: self.keyboard.wait(2)
            except subprocess.TimeoutExpired: self.keyboard.terminate(); self.keyboard.wait(2)
            self.keyboard.stdout.close(); self.keyboard=None
