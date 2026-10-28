"""Real host audio boundary; encoding/decoding takes place in the C3 image."""
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time
import wave

import numpy as np
import sounddevice as sd
from passport_paths import ffmpeg

ROOT=Path(__file__).resolve().parents[1]
HEADER=struct.Struct('<BII')  # type, C3 generation, payload bytes


class AudioBridge:
    def __init__(self):
        self.endpoint=('127.0.0.1',19546); self.token=None; self.force_reconnect=False
        self.connection=None; self.generation=0; self.mode=0; self.lock=threading.Lock()
        self.callback=None; self.event_callback=None; self.mic_token=0; self.mic=None; self.player=None; self.player_format=None; self.player_lock=threading.Lock()
        self.playback=True; self.stop=threading.Event(); self.play_queue=queue.Queue(96)
        self.stats={'packets_in':0,'packets_out':0,'pcm_bytes':0,'opus_bytes':0,'dropped':0,'errors':0,'underflows':0,'played_bytes':0}
        self.captured=bytearray(); self.play_thread=threading.Thread(target=self.play,daemon=True); self.play_thread.start()
        self.diagnostic_path=None;self.voice_captured=bytearray()

    def activate(self,mode,generation,callback=None):
        self.stop_microphone(); self.drain()
        self.mode=mode; self.generation=generation; self.callback=callback; self.event_callback=None
        if self.force_reconnect and self.connection is not None:
            with self.lock:
                old=self.connection; self.connection=None
                try: old.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                old.close()
        if self.connection is None:
            self.connection=socket.create_connection(self.endpoint,5); self.connection.settimeout(.2)
            if self.token: self.connection.sendall(self.token.encode('ascii'))
            threading.Thread(target=self.receive,daemon=True).start()

    def invalidate(self,generation):
        with self.lock: self.generation=generation
        self.stop_microphone(); self.drain()

    def send(self,kind,data,generation=None):
        if len(data)>4096: raise ValueError('native audio frame too large')
        with self.lock:
            if generation is not None and generation!=self.generation: return False
            if self.connection is None: raise RuntimeError('native audio socket not connected')
            self.connection.sendall(HEADER.pack(kind,self.generation,len(data))+data); self.stats['packets_in']+=1
            return True

    def receive(self):
        pending=bytearray(); connection=self.connection
        try:
            while not self.stop.is_set():
                try: chunk=connection.recv(16384)
                except socket.timeout: continue
                if not chunk: break
                pending.extend(chunk)
                while len(pending)>=HEADER.size:
                    kind,generation,length=HEADER.unpack_from(pending)
                    if length>8197: raise ValueError('invalid C3 audio frame')
                    if len(pending)<HEADER.size+length: break
                    data=bytes(pending[HEADER.size:HEADER.size+length]); del pending[:HEADER.size+length]
                    if generation!=self.generation: self.stats['dropped']+=1; continue
                    self.stats['packets_out']+=1
                    if kind==8 and self.diagnostic_path is not None and len(data)==3840 and len(self.voice_captured)<76800:
                        self.voice_captured.extend(data)
                        with os.fdopen(os.open(self.diagnostic_path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'wb') as destination:destination.write(self.voice_captured)
                    elif kind in (6,7):
                        callback=self.event_callback
                        if callback: callback(kind)
                    elif kind==1:
                        self.stats['opus_bytes']+=len(data)
                        callback=self.callback
                        if callback: callback(data)
                    elif kind==2 and len(data)>5:
                        rate,channels=struct.unpack_from('<IB',data); pcm=data[5:]
                        self.stats['pcm_bytes']+=len(pcm)
                        if len(self.captured)<2_000_000: self.captured.extend(pcm[:2_000_000-len(self.captured)])
                        if self.playback: self.enqueue_play(rate,channels,pcm)
                    elif kind==3:
                        self.stats['pcm_bytes']+=len(data)
                        callback=self.callback
                        if callback: callback(data)
        except (OSError,ValueError):
            if not self.stop.is_set() and self.connection is connection: self.stats['errors']+=1
        finally:
            with self.lock:
                if self.connection is connection:self.connection=None
            connection.close()

    def enqueue_play(self,rate,channels,pcm,device=None):
        try: self.play_queue.put_nowait((self.generation,rate,channels,pcm,device))
        except queue.Full: self.stats['dropped']+=1

    def play(self):
        pending=None
        while not self.stop.is_set():
            try:
                item=pending if pending is not None else self.play_queue.get(timeout=.2)
                pending=None; generation,rate,channels,pcm,device=item
            except queue.Empty: continue
            if generation!=self.generation: continue
            try:
                with self.player_lock:
                    if generation!=self.generation: continue
                    fmt=(rate,channels,device)
                    if fmt!=self.player_format:
                        # Prime 200ms before starting CoreAudio to absorb network
                        # and UART bursts without making an unbounded audio queue.
                        blocks=[pcm]; duration=len(pcm)/(rate*channels*2); until=time.monotonic()+.4
                        while duration<.2 and time.monotonic()<until:
                            try: next_item=self.play_queue.get(timeout=max(.001,until-time.monotonic()))
                            except queue.Empty: break
                            if next_item[:3]!=(generation,rate,channels) or next_item[4]!=device:
                                pending=next_item;break
                            blocks.append(next_item[3]);duration+=len(next_item[3])/(rate*channels*2)
                        pcm=b''.join(blocks)
                        if generation!=self.generation: continue
                        if self.player: self.player.stop(); self.player.close()
                        self.player=sd.RawOutputStream(samplerate=rate,channels=channels,dtype='int16',device=device)
                        self.player.start(); self.player_format=fmt
                    if self.player.write(pcm): self.stats['underflows']+=1
                    self.stats['played_bytes']+=len(pcm)
            except Exception: self.stats['errors']+=1

    def drain(self):
        while True:
            try: self.play_queue.get_nowait()
            except queue.Empty: break
        with self.player_lock:
            if self.player:
                self.player.abort(); self.player.close(); self.player=None; self.player_format=None

    def start_microphone(self,kind):
        self.stop_microphone(); self.mic_token+=1; token=self.mic_token; pending=bytearray(); generation=self.generation
        def transmit():
            while self.mic is not None and token==self.mic_token and not self.stop.is_set():
                try: block=self.mic_queue.get(timeout=.2)
                except queue.Empty: continue
                pending.extend(block)
                while len(pending)>=1920:
                    if token!=self.mic_token or not self.send(kind,bytes(pending[:1920]),generation): return
                    del pending[:1920]
        self.mic_queue=queue.Queue(50)
        def capture(data,frames,clock,status):
            block=np.frombuffer(data,dtype='<i2')[::3].tobytes()
            try: self.mic_queue.put_nowait(block)
            except queue.Full: self.stats['dropped']+=1
        self.mic=sd.RawInputStream(samplerate=48000,channels=1,dtype='int16',blocksize=2880,callback=capture)
        self.mic.start(); threading.Thread(target=transmit,daemon=True).start()

    def stop_microphone(self):
        self.mic_token+=1
        if self.mic:
            stream=self.mic; self.mic=None; stream.stop(); stream.close()

    def inject(self,path,kind=1,cancel=None):
        # A known test utterance, never ambient audio. ffmpeg only resamples;
        # all Opus encoding is performed by AudioPipe on the emulated C3.
        raw=subprocess.run([ffmpeg(),'-v','error','-i',str(path),'-f','s16le','-ar','16000','-ac','1','pipe:1'],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=20).stdout
        started=time.monotonic(); generation=self.generation
        for position in range(0,len(raw),1920):
            if cancel is not None and cancel.is_set(): raise RuntimeError('audio injection cancelled')
            frame=raw[position:position+1920].ljust(1920,b'\0')
            if not self.send(kind,frame,generation): raise RuntimeError('audio session changed')
            delay=started+(position+1920)/32000-time.monotonic()
            if delay>0: self.stop.wait(delay)
        return {'source_pcm_bytes':len(raw),'seconds':len(raw)/32000}

    def close(self):
        self.stop_microphone(); self.drain(); self.stop.set()
        if self.connection:
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            self.connection.close(); self.connection=None


class MediaPlayer:
    RADIO='https://ice1.somafm.com/groovesalad-128-mp3'
    def __init__(self,audio,fetcher):
        self.audio=audio; self.fetcher=fetcher; self.process=None; self.active=False
        self.details={}; self.stop=threading.Event(); self.update=None

    def start(self):
        self.close(); self.stop=threading.Event(); name,url='SomaFM Groove Salad',self.RADIO
        # The gateway normalizes containers to MP3. C3 performs the actual MP3
        # decode. Bounded pipes avoid downloading an entire episode into Flash.
        self.process=subprocess.Popen([ffmpeg(),'-v','error','-re','-i',url,'-vn',
            '-ac','1','-ar','24000','-c:a','libmp3lame','-b:a','64k','-flush_packets','1','-f','mp3','pipe:1'],
            stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        self.active=True; self.details={'name':name,'kind':'radio','compressed_bytes':0,'started':time.time()}
        process=self.process; cancel=self.stop; generation=self.audio.generation; update=self.update
        def feed():
            try:
                while not cancel.is_set():
                    data=process.stdout.read(1024)
                    if not data: break
                    if not self.audio.send(4,data,generation): break
                    first=self.details['compressed_bytes']==0
                    self.details['compressed_bytes']+=len(data)
                    if first and update: update({'state':'正在播放','value':'Radio',
                        'lines':[name[:80],'确认：播放 / 停止','','C3 原生 MP3 解码']})
            except (OSError,RuntimeError): pass
            if self.stop is cancel:
                self.active=False
                self.details['returncode']=process.poll()
                if not cancel.is_set() and update: update({'state':'播放失败' if process.poll() else '播放结束','value':'','lines':[name[:80],'确认重新播放']})
        threading.Thread(target=feed,daemon=True).start()
        return name

    def close(self):
        self.stop.set()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try: self.process.wait(3)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.process=None; self.active=False; self.audio.drain()
