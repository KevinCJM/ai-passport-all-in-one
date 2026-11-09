"""Bounded device RPC and dictation reports; no microphone or IME access."""
import asyncio
import collections
import json
import socket
import struct
import subprocess
import sys
import threading
import time
from passport_paths import helper

RPC_SERVICE='5d67d66a-70ed-4e30-9f6c-4fbddce3b020'
RPC_WRITE='5d67d66a-70ed-4e30-9f6c-4fbddce3b021'
RPC_READ='5d67d66a-70ed-4e30-9f6c-4fbddce3b022'
FRAGMENT=struct.Struct('<IHH')
AUDIO_HEADER=struct.Struct('<BII')
CAPACITY=2304


class LinkBusy(ConnectionError):
    pass


def start_gateway(port,forward,audio_forward,log,bridge=sys.platform=='win32'):
    """The USB PPP gateway on the badge's serial port: a process-like object (poll/terminate/wait/kill)."""
    command=[str(helper('passport-ppp')),'--forward',str(forward),'--audio-forward',str(audio_forward)]
    if not bridge:return subprocess.Popen(command+['--serial',port],stdout=log,stderr=subprocess.STDOUT)
    return SerialBridge(command,port,log)


class SerialBridge:
    """Windows: a COM port cannot be polled together with sockets, so the gateway listens on loopback and two threads
    copy bytes between it and the port. Ends (poll() returns the exit code) when either side does."""
    def __init__(self,command,port,log):
        import serial
        # The badge's USB Serial/JTAG ignores the baud rate. Windows opens COM ports exclusively; on a Mac, flock.
        self.serial=serial.Serial(None,115200,timeout=.2,write_timeout=2,exclusive=True);self.serial.port=port
        # Windows: pyserial raises DTR/RTS on open, and the USB Serial/JTAG reads an RTS edge as a chip reset, so the
        # badge drops off USB mid-pairing. The firmware ignores both lines; open with them low. (Mac: macOS raises both
        # on open anyway and pyserial clears DTR first, which would itself be the reset edge, so leave Mac as is.)
        if sys.platform=='win32':self.serial.dtr=self.serial.rts=False
        self.serial.open();self.socket=None;self.closed=threading.Event()
        try:
            with socket.socket() as probe:probe.bind(('127.0.0.1',0));free=probe.getsockname()[1]
            self.process=subprocess.Popen(command+['--port',str(free)],stdout=log,stderr=subprocess.STDOUT)
            until=time.monotonic()+5
            while self.socket is None:
                try:self.socket=socket.create_connection(('127.0.0.1',free),1)
                except OSError:
                    if self.process.poll() is not None or time.monotonic()>until:raise ConnectionError('PPP gateway did not start')
                    time.sleep(.1)
        except BaseException:
            self.terminate();raise
        for target in (self.upstream,self.downstream):threading.Thread(target=target,daemon=True).start()

    def upstream(self):
        try:
            while not self.closed.is_set():
                data=self.serial.read(self.serial.in_waiting or 1)
                if data:self.socket.sendall(data)
        except Exception:pass
        self.closed.set()

    def downstream(self):
        try:
            while not self.closed.is_set():
                data=self.socket.recv(4096)
                if not data:break
                self.serial.write(data)
        except Exception:pass
        self.closed.set()

    def poll(self):
        # Either side gone ends both: the port is freed before the device starts a new gateway on it.
        if self.closed.is_set() or self.process.poll() is not None:
            self.terminate()
            try:return self.process.wait(3)
            except subprocess.TimeoutExpired:self.process.kill();return self.process.wait()
        return self.process.poll()

    def terminate(self):
        self.closed.set()
        if self.socket:
            try:self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            self.socket.close()
        self.serial.close()
        if getattr(self,'process',None) and self.process.poll() is None:self.process.terminate()

    def wait(self,timeout=None):return self.process.wait(timeout)

    def kill(self):self.process.kill()


async def ble_request(client,identity,token,path,data=None):
    envelope={'token':token,'path':path}
    if data is not None:envelope['data']=data
    raw=json.dumps(envelope,ensure_ascii=False,separators=(',',':')).encode()
    if not 0<len(raw)<CAPACITY:raise ValueError('device request too large')
    result=bytearray();total=None
    async with asyncio.timeout(10):
        for offset in range(0,len(raw),172):
            await client.write_gatt_char(RPC_WRITE,FRAGMENT.pack(identity,offset,len(raw))+raw[offset:offset+172],response=True)
        while True:
            packet=await client.read_gatt_char(RPC_READ)
            if len(packet)<FRAGMENT.size:raise ValueError('short BLE response')
            received,offset,length=FRAGMENT.unpack_from(packet)
            if received!=identity:
                if result:raise ValueError('BLE response changed during transfer')
                await asyncio.sleep(.03);continue
            if not 0<length<CAPACITY or offset!=len(result) or total not in (None,length):raise ValueError('invalid BLE response sequence')
            part=packet[FRAGMENT.size:]
            if not part or len(result)+len(part)>length:raise ValueError('invalid BLE response size')
            total=length;result.extend(part)
            if len(result)==total:break
    reply=json.loads(result)
    if reply.get('status')==503:raise LinkBusy('device service queue busy')
    if path=='/menu' and reply.get('status')==409:raise ValueError('配置已变化或正在保存，请重新读取胸牌')
    if reply.get('status')!=200:raise ConnectionError('BLE API status '+str(reply.get('status')))
    return reply['body']


class UsbReports:
    def __init__(self,endpoint,token):
        self.endpoint=endpoint;self.token=token;self.socket=None;self.thread=None
        self.lock=threading.Lock();self.send_lock=threading.Lock();self.events=collections.deque(maxlen=1024)
        self.dropped=0;self.generation=0
        self.ready=threading.Event()

    @property
    def connected(self):return self.socket is not None and self.ready.is_set()

    def open(self,generation):
        if self.connected and self.generation==generation:return
        self.close();connection=socket.create_connection(self.endpoint,5);connection.settimeout(.2)
        connection.sendall(self.token.encode('ascii'));self.generation=generation;self.socket=connection
        self.thread=threading.Thread(target=self.receive,args=(connection,generation),daemon=True);self.thread.start()
        if not self.ready.wait(3):self.close();raise TimeoutError('USB authentication acknowledgement missing')

    def receive(self,connection,generation):
        pending=bytearray()
        try:
            while self.socket is connection:
                try:chunk=connection.recv(4096)
                except socket.timeout:continue
                if not chunk:break
                pending.extend(chunk)
                while len(pending)>=AUDIO_HEADER.size:
                    kind,owner,length=AUDIO_HEADER.unpack_from(pending)
                    if kind not in (10,11,13) or length!={10:8,11:176,13:0}[kind]:raise ValueError('invalid USB report')
                    if len(pending)<AUDIO_HEADER.size+length:break
                    raw=bytes(pending[AUDIO_HEADER.size:AUDIO_HEADER.size+length]);del pending[:AUDIO_HEADER.size+length]
                    if owner!=generation:continue
                    if kind==13:self.ready.set();continue
                    with self.lock:
                        if len(self.events)==self.events.maxlen:self.dropped+=1
                        self.events.append({'report':1 if kind==10 else 2,'hex':raw.hex()})
        except (OSError,ValueError):pass
        finally:
            if self.socket is connection:self.socket=None;self.ready.clear()
            with self.lock:self.events.append({'report':0,'hex':''})
            connection.close()

    def clear(self):
        with self.lock:self.events.clear();self.dropped=0

    def poll(self):
        with self.lock:
            events=list(self.events);self.events.clear();dropped=self.dropped;self.dropped=0
        return {'connected':self.connected,'events':events,'dropped':dropped}

    def ack(self,kind,generation,session):
        raw=struct.pack('<BII',kind,generation,session).ljust(16,b'\0')
        with self.send_lock:
            if not self.socket:raise ConnectionError('USB audio disconnected')
            self.socket.sendall(AUDIO_HEADER.pack(12,generation,len(raw))+raw)

    def close(self):
        connection=self.socket;self.socket=None;self.ready.clear()
        if connection:
            try:connection.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            connection.close()
        if self.thread and self.thread is not threading.current_thread():self.thread.join(2)
        self.thread=None;self.clear()
