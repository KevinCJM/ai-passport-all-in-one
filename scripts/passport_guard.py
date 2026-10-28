"""离座锁屏: the badge's Bluetooth signal, as this Mac reads it, tells whether you are at the desk.

While you are near, the Mac stays awake (防锁屏: caffeinate; 保持在线 also nudges the mouse a pixel so chat apps do
not show you away). Once the signal stays below the line for the chosen delay, or the link drops for that long, the
screen locks. When you come back the screen can light up; unlocking stays yours (password or Touch ID).

The badge sends its choices as one operation, "<verb>:DLKW": distance 0 近 1 中 2 远, delay index into DELAYS, keep
0 off / 1 display / 2 online, wake 0/1. Verbs: cfg (settings), cal (take the current signal as the at-desk level),
lock (lock now). Over USB there is no signal: only 防锁屏 and 立即锁屏 work.
"""
import ctypes
import os
import statistics
import subprocess
import time

DELAYS=(5,10,20,30)
OFFSETS=(8,12,18)       # dB under the calibrated level: 近 / 中 / 远
ABSOLUTE=(-65,-72,-80)  # the same, before any calibration
RETURN_MARGIN=4         # dB above the line before a locked badge counts as back
NUDGE_IDLE=180          # seconds without input before 保持在线 nudges the mouse
# Phases, as the badge shows them.
WAITING,USB,PRESENT,LEAVING,LOCKED=range(5)


def lock_screen():
    ctypes.CDLL('/System/Library/PrivateFrameworks/login.framework/login').SACLockScreenImmediate()


def screen_locked():
    """Whether the login window covers the session (CGSSessionScreenIsLocked)."""
    cg=_graphics()
    cf=ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    cg.CGSessionCopyCurrentDictionary.restype=ctypes.c_void_p
    cf.CFStringCreateWithCString.restype=ctypes.c_void_p;cf.CFStringCreateWithCString.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_uint32]
    cf.CFDictionaryGetValue.restype=ctypes.c_void_p;cf.CFDictionaryGetValue.argtypes=[ctypes.c_void_p,ctypes.c_void_p]
    cf.CFBooleanGetValue.argtypes=[ctypes.c_void_p];cf.CFRelease.argtypes=[ctypes.c_void_p]
    key=cf.CFStringCreateWithCString(None,b'CGSSessionScreenIsLocked',0x08000100);session=cg.CGSessionCopyCurrentDictionary()
    try:
        value=cf.CFDictionaryGetValue(session,key) if session else None
        return bool(value) and bool(cf.CFBooleanGetValue(value))
    finally:
        if session:cf.CFRelease(session)
        cf.CFRelease(key)


def wake_display():
    subprocess.Popen(['caffeinate','-u','-t','2'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)


class _Point(ctypes.Structure):
    _fields_=[('x',ctypes.c_double),('y',ctypes.c_double)]


_cg=None


def _graphics():
    global _cg
    if _cg is None:
        cg=ctypes.CDLL('/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices')
        cg.CGEventCreate.restype=ctypes.c_void_p;cg.CGEventCreate.argtypes=[ctypes.c_void_p]
        cg.CGEventGetLocation.restype=_Point;cg.CGEventGetLocation.argtypes=[ctypes.c_void_p]
        cg.CGEventCreateMouseEvent.restype=ctypes.c_void_p
        cg.CGEventCreateMouseEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint32,_Point,ctypes.c_uint32]
        cg.CGEventPost.argtypes=[ctypes.c_uint32,ctypes.c_void_p];cg.CFRelease.argtypes=[ctypes.c_void_p]
        cg.CGEventSourceSecondsSinceLastEventType.restype=ctypes.c_double
        cg.CGEventSourceSecondsSinceLastEventType.argtypes=[ctypes.c_uint32,ctypes.c_uint32]
        _cg=cg
    return _cg


def idle_seconds():
    return _graphics().CGEventSourceSecondsSinceLastEventType(1,0xFFFFFFFF)  # HID system state, any input


def nudge_mouse():
    """One pixel right and back: resets the system idle time (needs the Accessibility permission)."""
    cg=_graphics();event=cg.CGEventCreate(None);point=cg.CGEventGetLocation(event);cg.CFRelease(event)
    for dx in (1,-1):
        move=cg.CGEventCreateMouseEvent(None,5,_Point(point.x+dx,point.y),0);cg.CGEventPost(0,move);cg.CFRelease(move)


class Awake:
    """caffeinate -d -i while wanted; it also ends with this process."""
    def __init__(self):self.process=None
    def hold(self,wanted):
        if wanted and (self.process is None or self.process.poll() is not None):
            self.process=subprocess.Popen(['caffeinate','-d','-i','-w',str(os.getpid())],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        elif not wanted and self.process is not None:
            self.process.terminate();self.process=None


class Guard:
    """One step a second (tick). rssi() returns dBm, None when the Bluetooth link is down, or 'usb'."""
    def __init__(self,rssi,settings,save,lock=lock_screen,wake=wake_display,nudge=nudge_mouse,idle=idle_seconds,
                 awake=None,now=time.monotonic,locked=screen_locked):
        self.rssi=rssi;self.settings=settings;self.save=save;self.lock=lock;self.wake=wake;self.nudge=nudge
        self.idle=idle;self.awake=awake or Awake();self.now=now;self.locked=locked;self.locked_at=0;self.woken=False
        self.distance,self.delay,self.keep,self.wake_back=1,1,2,1
        self.samples=[];self.phase=WAITING;self.below_since=None;self.above_since=None;self.reading=None
        self.last_nudge=0;self.locks=0;self.events=[]

    @property
    def baseline(self):return self.settings.get('guard_baseline')

    def line(self):
        return self.baseline-OFFSETS[self.distance] if self.baseline is not None else ABSOLUTE[self.distance]

    def configure(self,operation):
        verb,_,values=operation.partition(':')
        if len(values)==4 and values.isdigit():
            d,l,k,w=(int(c) for c in values)
            if d<3 and l<4 and k<3 and w<2:self.distance,self.delay,self.keep,self.wake_back=d,l,k,w
        if verb=='cal' and self.reading is not None:
            self.settings['guard_baseline']=round(self.reading);self.save();self.events.append('calibrated')
        if verb=='lock':self.lock_now('manual')

    def lock_now(self,why):
        if self.settings.get('guard_dry_run'):self.events.append('would lock: '+why)
        else:self.lock()
        self.locks+=1;self.phase=LOCKED;self.below_since=None;self.above_since=None;self.locked_at=self.now();self.woken=False

    def tick(self):
        now=self.now();value=self.rssi()
        if value=='usb':self.samples=[];self.reading=None
        else:
            if value is None:self.samples=[]
            else:self.samples=(self.samples+[value])[-5:]
            self.reading=statistics.median(self.samples) if self.samples else None
        line=self.line()
        if self.phase==LOCKED:
            # Back when the signal stays above the line, with a margin, for three seconds: light the screen once.
            if self.reading is not None and self.reading>=line+RETURN_MARGIN:
                self.above_since=self.above_since or now
                if now-self.above_since>=3 and not self.woken:
                    self.woken=True
                    if self.wake_back:self.wake()
            else:self.above_since=None
            # Locked until you unlock (a dry run pretends: until you are back, or 5 s on USB).
            if self.settings.get('guard_dry_run'):still=not self.woken and (value!='usb' or now-self.locked_at<5)
            else:still=self.locked()
            if not still:self.phase=WAITING;self.above_since=None
        if self.phase==LOCKED:pass
        elif value=='usb':self.below_since=None;self.phase=USB
        else:
            if self.reading is None or self.reading<line:
                self.below_since=self.below_since or now
                self.phase=LEAVING
                if now-self.below_since>=DELAYS[self.delay]:self.lock_now('away' if self.reading is not None else 'link lost')
            else:
                self.below_since=None;self.phase=PRESENT
        # 防锁屏 only while you are here (or on USB, where the badge is at the desk by its cable), and not over a
        # screen that is already locked.
        here=self.phase in (PRESENT,USB) and not self.locked()
        self.awake.hold(here and self.keep>0)
        if here and self.keep==2 and now-self.last_nudge>=60 and self.idle()>=NUDGE_IDLE:
            self.nudge();self.last_nudge=now
        return self.state()

    def left(self):
        if self.phase!=LEAVING or self.below_since is None:return 0
        return max(0,int(DELAYS[self.delay]-(self.now()-self.below_since)+.999))

    def state(self):
        names=('等待连接','USB 连接中','在座','即将锁屏','已锁屏')
        reading=None if self.reading is None else round(self.reading)
        return {'state':names[self.phase],'value':'','lines':[],
                'guard':{'phase':self.phase,'rssi':reading or 0,'line':self.line(),'left':self.left(),
                         'base':self.baseline or 0,'locks':self.locks}}

    def stop(self):
        self.awake.hold(False);self.phase=WAITING;self.samples=[]
