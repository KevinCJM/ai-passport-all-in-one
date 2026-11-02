"""The assistant's Windows side, ctypes only: clipboard, key presses, screen lock, idle time, keep-awake, autostart.

Each function is the Windows twin of a Mac one (passport_dictation.paste/keys, passport_guard, the LaunchAgent).
Importable anywhere; the DLLs load on first use, so the key plans can be tested on a Mac.
"""
import ctypes
import sys
import time

# Virtual-key codes.
CONTROL,SHIFT,RETURN,ESCAPE,BACK,HOME,END,V=0x11,0x10,0x0D,0x1B,0x08,0x24,0x23,0x56
EXTENDED={HOME,END}  # navigation keys carry KEYEVENTF_EXTENDEDKEY like a real keyboard's
# Foreground programs whose input line is a shell's: Esc clears it there (cmd, PowerShell's PSReadLine).
# ponytail: bash/WSL inside Windows Terminal ignores Esc; telling the shell apart needs the console's child process.
TERMINALS={'windowsterminal.exe','openconsole.exe','conhost.exe','cmd.exe','powershell.exe','pwsh.exe'}
KEYS={'paste':[(CONTROL,V)],'enter':[(RETURN,)],
      # Text fields: to the end of the line, select back to its start, delete. Same reach as Command-Backspace.
      'delete_line':[(END,),(SHIFT,HOME),(BACK,)]}
RUN_KEY=r'Software\Microsoft\Windows\CurrentVersion\Run'
RUN_NAME='AI Passport Assistant'


def plan(action,terminal=False):
    """The key chords for one of 语音输入's actions."""
    return [(ESCAPE,)] if action=='delete_line' and terminal else KEYS[action]


_api=None


def api():
    """user32/kernel32 with the argument types set (64-bit handles must not be truncated)."""
    global _api
    if _api is None:
        from ctypes import wintypes as w
        user32=ctypes.WinDLL('user32',use_last_error=True);kernel32=ctypes.WinDLL('kernel32',use_last_error=True)
        handle=ctypes.c_void_p
        for name,result,arguments in [
            (user32.OpenClipboard,w.BOOL,[w.HWND]),(user32.CloseClipboard,w.BOOL,[]),(user32.EmptyClipboard,w.BOOL,[]),
            (user32.GetClipboardData,handle,[w.UINT]),(user32.SetClipboardData,handle,[w.UINT,handle]),
            (user32.RegisterClipboardFormatW,w.UINT,[w.LPCWSTR]),(user32.GetClipboardSequenceNumber,w.DWORD,[]),
            (user32.SendInput,w.UINT,[w.UINT,ctypes.c_void_p,ctypes.c_int]),(user32.GetForegroundWindow,w.HWND,[]),
            (user32.GetWindowThreadProcessId,w.DWORD,[w.HWND,ctypes.POINTER(w.DWORD)]),(user32.LockWorkStation,w.BOOL,[]),
            (user32.GetLastInputInfo,w.BOOL,[ctypes.c_void_p]),(kernel32.GetTickCount,w.DWORD,[]),
            (kernel32.GlobalAlloc,handle,[w.UINT,ctypes.c_size_t]),(kernel32.GlobalLock,ctypes.c_void_p,[handle]),
            (kernel32.GlobalUnlock,w.BOOL,[handle]),(kernel32.GlobalFree,handle,[handle]),
            (kernel32.SetThreadExecutionState,w.DWORD,[w.DWORD])]:
            name.restype=result;name.argtypes=arguments
        _api=(user32,kernel32)
    return _api


class _Keyboard(ctypes.Structure):
    _fields_=[('vk',ctypes.c_ushort),('scan',ctypes.c_ushort),('flags',ctypes.c_ulong),('time',ctypes.c_ulong),('extra',ctypes.c_size_t)]


class _Mouse(ctypes.Structure):
    _fields_=[('dx',ctypes.c_long),('dy',ctypes.c_long),('data',ctypes.c_ulong),('flags',ctypes.c_ulong),('time',ctypes.c_ulong),('extra',ctypes.c_size_t)]


class _Input(ctypes.Structure):
    class _Union(ctypes.Union):
        _fields_=[('ki',_Keyboard),('mi',_Mouse),('pad',ctypes.c_byte*32)]  # pad: the union's real size (HARDWAREINPUT etc.)
    _fields_=[('type',ctypes.c_ulong),('u',_Union)]


def _send(events):
    array=(_Input*len(events))(*events)
    if api()[0].SendInput(len(events),array,ctypes.sizeof(_Input))!=len(events):
        raise PermissionError('Windows blocked the key press')


def press(chords):
    for chord in chords:
        events=[]
        for up in (False,True):
            for vk in (reversed(chord) if up else chord):
                event=_Input(type=1);event.u.ki=_Keyboard(vk=vk,flags=(2 if up else 0)|(1 if vk in EXTENDED else 0));events.append(event)
        _send(events)


def foreground_terminal():
    import psutil
    from ctypes import wintypes
    user32=api()[0];pid=wintypes.DWORD()
    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(),ctypes.byref(pid))
    try:return psutil.Process(pid.value).name().lower() in TERMINALS
    except Exception:return False


def keys(action):
    """passport_dictation's keyboard: 'paste', 'enter' or 'delete_line' into the foreground window."""
    press(plan(action,action=='delete_line' and foreground_terminal()))


# Clipboard: text only. ponytail: pictures or files on the clipboard are not put back after a paste.
def _open_clipboard():
    for _ in range(40):  # another program may hold it for a moment
        if api()[0].OpenClipboard(None):return
        time.sleep(.025)
    raise OSError('clipboard busy')


def _put(kind,raw):
    user32,kernel32=api();memory=kernel32.GlobalAlloc(2,len(raw))  # GMEM_MOVEABLE
    pointer=kernel32.GlobalLock(memory);ctypes.memmove(pointer,raw,len(raw));kernel32.GlobalUnlock(memory)
    if not user32.SetClipboardData(kind,memory):kernel32.GlobalFree(memory);raise OSError('clipboard write failed')


def get_text():
    user32,kernel32=api();_open_clipboard()
    try:
        memory=user32.GetClipboardData(13)  # CF_UNICODETEXT
        if not memory:return None
        pointer=kernel32.GlobalLock(memory)
        try:return ctypes.wstring_at(pointer)
        finally:kernel32.GlobalUnlock(memory)
    finally:user32.CloseClipboard()


def set_text(text,private=False):
    """private: kept out of clipboard history, cloud clipboard and clipboard managers (like macOS TransientType)."""
    user32=api()[0];_open_clipboard()
    try:
        user32.EmptyClipboard();_put(13,(text+'\0').encode('utf-16-le'))
        if private:
            for name,raw in (('ExcludeClipboardContentFromMonitorProcessing',b'\0'),('CanIncludeInClipboardHistory',bytes(4)),('CanUploadToCloudClipboard',bytes(4))):
                _put(user32.RegisterClipboardFormatW(name),raw)
    finally:user32.CloseClipboard()


def clipboard_sequence():
    return api()[0].GetClipboardSequenceNumber()


# 离座锁屏
def lock_screen():
    api()[0].LockWorkStation()


def screen_locked():
    """The lock screen is up while LogonUI runs."""
    import psutil
    return any(p.info['name']=='LogonUI.exe' for p in psutil.process_iter(['name']))


def wake_display():
    api()[1].SetThreadExecutionState(2)  # ES_DISPLAY_REQUIRED, once


def idle_seconds():
    class Info(ctypes.Structure):_fields_=[('size',ctypes.c_uint),('time',ctypes.c_ulong)]
    user32,kernel32=api();info=Info(ctypes.sizeof(Info))
    user32.GetLastInputInfo(ctypes.byref(info))
    return ((kernel32.GetTickCount()-info.time)&0xFFFFFFFF)/1000


def nudge_mouse():
    """One pixel right and back: resets the idle time."""
    events=[]
    for dx in (1,-1):
        event=_Input(type=0);event.u.mi=_Mouse(dx=dx,flags=1);events.append(event)  # MOUSEEVENTF_MOVE, relative
    _send(events)


class Awake:
    """Display and system stay on while wanted. The request belongs to the calling thread (Guard's loop), and ends
    with it as well."""
    def hold(self,wanted):
        api()[1].SetThreadExecutionState(0x80000000|(3 if wanted else 0))  # ES_CONTINUOUS | SYSTEM | DISPLAY


def autostart(enable=None):
    """Start with Windows: the per-user Run key, no administrator rights. None reads the current state."""
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER,RUN_KEY,0,winreg.KEY_READ|winreg.KEY_SET_VALUE) as key:
        if enable is None:
            try:return winreg.QueryValueEx(key,RUN_NAME)[0].startswith('"'+sys.executable+'"')
            except FileNotFoundError:return False
        if enable:winreg.SetValueEx(key,RUN_NAME,0,winreg.REG_SZ,f'"{sys.executable}" --background')
        else:
            try:winreg.DeleteValue(key,RUN_NAME)
            except FileNotFoundError:pass
        return enable
