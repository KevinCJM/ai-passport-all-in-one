"""AI 控制器: the recent Claude Code and Codex conversations for the badge, read from their own records on this Mac,
and the way back to one of them: its terminal tab, a resumed one in a new tab, or the Codex app.

Claude Code: ~/.claude/projects/<folder>/<session>.jsonl. Codex: ~/.codex/state_5.sqlite (threads) and each thread's
rollout jsonl. Both formats are undocumented; what cannot be read is left out.
"""
import json
import os
import re
import shlex
import sqlite3
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

HOME=Path.home()
TOOLS=('codex','claude')        # the badge's AI 控制器 rows 0 and 1
DAY=86400
STALE=900                       # a turn with no new record for 15 minutes was stopped (terminal closed, crash)
FIRST_READ=2<<20                # a long record is read from its last 2 MB the first time (Codex rollouts reach 70 MB)
TERMINALS=('Ghostty','Warp','Terminal','iTerm')
RESUME={'claude':'claude --resume %s','codex':'codex resume %s'}
DEFAULTS={'codex':'app','claude':'terminal','terminal':'Ghostty'}
# State codes on the badge: 0 idle, 1 running, 2 replied (read), 3 replied and not opened yet.
IDLE,RUN,DONE,UNREAD=range(4)


def stamp(value):
    try:return datetime.fromisoformat(value.replace('Z','+00:00')).timestamp()
    except (AttributeError,ValueError):return 0


def text_of(content):
    if isinstance(content,str):return content
    if isinstance(content,list):return '\n'.join(x.get('text','') for x in content if isinstance(x,dict) and x.get('type') in ('text','output_text','input_text'))
    return ''


def plain(text,size):
    """Markdown flattened to one line for the badge, cut to size UTF-8 bytes on a character boundary."""
    text=re.sub(r'```.*?(```|$)',' ',text or '',flags=re.S)
    text=re.sub(r'\[([^\]]*)\]\([^)]*\)',r'\1',text)
    text=re.sub(r'[*_`#>|]+|^\s*[-+]\s+',' ',text,flags=re.M)
    text=re.sub(r'\s+',' ',text).strip()
    return text.encode()[:size].decode('utf-8','ignore')


class Tail:
    """A growing jsonl file: each call yields only the records added since the last one."""
    def __init__(self,path):
        self.path=path;self.offset=None;self.rest=b''

    def records(self):
        size=os.path.getsize(self.path)
        if self.offset is None or size<self.offset:self.offset=max(0,size-FIRST_READ);self.rest=b'' if not self.offset else None
        if size==self.offset:return
        with open(self.path,'rb') as f:
            f.seek(self.offset);data=f.read(size-self.offset);self.offset=size
        if self.rest is None:data=data.partition(b'\n')[2];self.rest=b''   # started mid-line
        *lines,self.rest=(self.rest+data).split(b'\n')
        for line in lines:
            try:yield json.loads(line)
            except ValueError:pass


def claude_record(s,d):
    kind=d.get('type')
    if kind=='ai-title':s['auto']=d.get('aiTitle') or s.get('auto')
    elif kind=='custom-title':s['custom']=d.get('customTitle') or s.get('custom')
    if kind not in ('user','assistant') or d.get('isSidechain') or d.get('isMeta'):return
    s['cwd']=s.get('cwd') or d.get('cwd');m=d.get('message');m=m if isinstance(m,dict) else {};content=m.get('content')
    if kind=='user':
        if isinstance(content,list) and content and all(isinstance(x,dict) and x.get('type')=='tool_result' for x in content):s['state']=RUN
        else:
            said=text_of(content).strip()
            if said.startswith('[Request interrupted'):s['state']=IDLE
            elif not said or said.startswith('<'):return   # slash commands and their output
            else:s['state']=RUN;s['asked']=s.get('asked') or said
    elif m.get('stop_reason')=='end_turn':s['state']=DONE;s['reply']=text_of(content)
    else:s['state']=RUN
    s['at']=stamp(d.get('timestamp')) or s.get('at',0)


def codex_record(s,d):
    p=d.get('payload') or {};kind=p.get('type')
    if d.get('type')=='response_item' and kind=='message' and p.get('role')=='assistant':s['said']=text_of(p.get('content'));return
    if d.get('type')!='event_msg' or kind not in ('task_started','task_complete','turn_aborted'):return
    s['state']={'task_started':RUN,'task_complete':DONE,'turn_aborted':IDLE}[kind]
    if kind=='task_complete':s['reply']=p.get('last_agent_message') or s.get('said','')
    s['at']=stamp(d.get('timestamp')) or s.get('at',0)


def applescript(source):
    return subprocess.run(['osascript','-e',source],capture_output=True,text=True,timeout=10).stdout.strip()


def quoted(text):
    return '"'+text.replace('\\','\\\\').replace('"','\\"')+'"'


class AISessions:
    def __init__(self,settings,save,read_path,typist=None):
        self.settings=settings;self.save=save;self.read_path=read_path;self.typist=typist
        self.tails={};self.found={};self.target=None;self.lock=threading.Lock()   # the badge's requests and its aigo events
        try:self.read=json.loads(read_path.read_text())
        except (OSError,ValueError):self.read=None   # first run: everything already finished counts as read

    def config(self):
        return dict(DEFAULTS,**self.settings.get('ai_open',{}))

    def configure(self,data):
        clean={}
        for tool in TOOLS:
            if data.get(tool) not in ('app','terminal'):raise ValueError(tool)
            clean[tool]=data[tool]
        if clean['claude']=='app':raise ValueError('claude')   # Claude's app cannot open a terminal session (yet)
        if data.get('terminal') not in TERMINALS:raise ValueError('terminal')
        clean['terminal']=data['terminal'];self.settings['ai_open']=clean;self.save()
        return clean

    def session(self,key,path,parse,base):
        found=self.found.get(key)
        if found is None or found['path']!=path:found=self.found[key]=dict(base,path=path,state=IDLE,at=0)
        found.update(base)
        tail=self.tails.get(path)
        if tail is None:tail=self.tails[path]=Tail(path)
        for record in tail.records():parse(found,record)
        return found

    def claude(self,now):
        out=[]
        for path in (HOME/'.claude'/'projects').glob('*/*.jsonl'):
            try:modified=path.stat().st_mtime
            except OSError:continue
            if now-modified>DAY:continue
            s=self.session('claude:'+path.stem,str(path),claude_record,{'tool':'claude','id':path.stem,'modified':modified,'app':False})
            if not s.get('asked') and s['state']==IDLE:continue   # nothing asked yet
            s['title']=s.get('custom') or s.get('auto') or s.get('asked','')
            out.append(s)
        return out

    def codex(self,now):
        out=[]
        try:
            with sqlite3.connect(f'file:{HOME}/.codex/state_5.sqlite?mode=ro',uri=True,timeout=2) as db:
                rows=db.execute("select id,rollout_path,cwd,coalesce(nullif(name,''),title),originator from threads "
                                "where thread_source='user' and archived=0 and updated_at>? order by updated_at desc limit 12",(int(now-DAY),)).fetchall()
        except sqlite3.Error:return out
        for thread,path,cwd,title,originator in rows:
            try:modified=os.path.getmtime(path)
            except (OSError,TypeError):continue
            s=self.session('codex:'+thread,path,codex_record,{'tool':'codex','id':thread,'cwd':cwd,'modified':modified,
                                                               'title':(title or '').split('\n')[0],'app':originator=='Codex Desktop'})
            out.append(s)
        return out

    def scan(self):
        """{tool: its conversations, newest first}, each with its badge state code."""
        with self.lock:return self.scan_locked()

    def scan_locked(self):
        now=time.time();result={'codex':self.codex(now),'claude':self.claude(now)}
        first=self.read is None
        if first:self.read={}
        for found in result.values():
            for s in found:
                if s['state']==RUN and now-s['modified']>STALE:s['state']=IDLE
                if first and s['state']==DONE:self.read[s['id']]=now
                s['code']=s['state'] if s['state']!=DONE or self.read.get(s['id'],0)>=s['at'] else UNREAD
            found.sort(key=lambda s:s['at'] or s['modified'],reverse=True)
        if first:self.keep_read()
        return result

    def keep_read(self):
        live={s['id'] for s in self.found.values()}
        self.read={k:v for k,v in self.read.items() if k in live}
        try:self.read_path.write_text(json.dumps(self.read))
        except OSError:pass

    def badge(self,tool=None):
        """The badge's part: per tool [replied and unread, running]; with a tool, its five latest conversations as
        [id8, title, folder, state, minutes ago, reply head], unread ones first. Sized for the badge's 2 KB message."""
        found=self.scan();now=time.time()
        rows=[[sum(s['code']==UNREAD for s in found[t]),sum(s['code']==RUN for s in found[t])] for t in TOOLS]
        out={'rows':rows}
        if tool in TOOLS:
            listed=sorted(found[tool],key=lambda s:s['code']!=UNREAD)[:5]   # stable: newest first within each group
            out['list']=[[s['id'][:8],plain(s['title'],36) or '未命名对话',plain(Path(s.get('cwd') or '').name,18),s['code'],
                          min(9999,int((now-(s['at'] or s['modified']))//60)),plain(s.get('reply',''),120)] for s in listed]
        return out

    def find(self,tool,prefix):
        if len(prefix)<8:return None
        with self.lock:return next((s for s in self.found.values() if s['tool']==tool and s['id'].startswith(prefix)),None)

    # The computer side ------------------------------------------------------------------------------------------

    def open(self,tool,prefix):
        """Brings the conversation up with its input focused; returns the badge's note."""
        s=self.find(tool,prefix)
        if s is None:return '这个对话已不在列表里'
        with self.lock:self.read[s['id']]=time.time();self.keep_read()
        self.target=s
        return self.show(s,resume=True)

    def refocus(self):
        """Before 语音输入 types into the conversation opened from the badge: back to it, whatever has the focus now."""
        if self.target is not None:self.show(self.target,resume=False);time.sleep(.3)

    def show(self,s,resume):
        config=self.config()
        if s['tool']=='codex' and config['codex']=='app':
            subprocess.run(['open','codex://threads/'+s['id']],timeout=8,check=True);return '已在 Codex 打开'
        where=self.focus(s)
        if where:return '已切到 '+where
        if not resume:return ''
        return self.resume(s,config['terminal'])

    def process(self,s):
        """The terminal app running this conversation's CLI ('' when none does), and the CLI's tty."""
        table={}
        for line in subprocess.run(['ps','-axo','pid=,ppid=,tty=,args='],capture_output=True,text=True,timeout=5).stdout.splitlines():
            pid,ppid,tty,args=(line.split(None,3)+['','',''])[:4]
            table[pid]=(ppid,tty,args)
        name=s['tool']
        mine=[pid for pid,(_,_,args) in table.items() if Path(args.split(' ')[0]).name==name and '.app/' not in args.split(' ')[0]]
        if not mine:return '',''
        cwd={}
        out=subprocess.run(['lsof','-a','-d','cwd','-Fn','-p',','.join(mine)],capture_output=True,text=True,timeout=5).stdout
        for pid,folder in re.findall(r'^p(\d+)\n(?:f\w+\n)?n(.+)$',out,re.M):cwd[pid]=folder
        # ponytail: two CLIs of the same tool in the same folder are told apart only by a --resume id in their arguments
        picked=[pid for pid in mine if s['id'] in table[pid][2]] or [pid for pid in mine if cwd.get(pid)==s.get('cwd')]
        if not picked:return '',''
        pid=picked[0];tty=table[pid][1];hops=0
        while pid in table and hops<20:
            program=table[pid][2].split(' ')[0]
            if '.app/Contents/MacOS/' in program:return program.split('.app/')[0].rsplit('/',1)[-1],tty
            pid=table[pid][0];hops+=1
        return '',tty

    def focus(self,s):
        app,tty=self.process(s)
        if not app:return ''
        title=plain(s['title'],60)
        if app=='Ghostty':
            # The tab whose title shows the conversation's (Claude sets it), else the first one in its folder.
            applescript(f'''tell application "Ghostty"
  set picked to missing value
  repeat with t in terminals
    if name of t contains {quoted(title)} then set picked to t
  end repeat
  if picked is missing value then
    repeat with t in terminals
      if working directory of t is {quoted(s.get('cwd') or '')} then set picked to t
    end repeat
  end if
  if picked is not missing value then focus picked
  activate
end tell''')
        elif app=='Terminal':
            applescript(f'''tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "/dev/{tty}" then
        set selected of t to true
        set index of w to 1
      end if
    end repeat
  end repeat
  activate
end tell''')
        elif app in ('iTerm','iTerm2'):
            applescript(f'''tell application "iTerm"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with c in sessions of t
        if tty of c is "/dev/{tty}" then
          select w
          tell t to select
          tell c to select
        end if
      end repeat
    end repeat
  end repeat
  activate
end tell''')
        else:
            # Warp and the rest: only windows can be raised, by the title of their selected tab.
            subprocess.run(['open','-a',app],timeout=8)
            if title:applescript(f'''tell application "System Events" to repeat with p in (processes whose bundle identifier is (id of application {quoted(app)}))
  repeat with w in windows of p
    if name of w contains {quoted(title)} then perform action "AXRaise" of w
  end repeat
end repeat''')
        return app

    def resume(self,s,terminal):
        """No CLI runs the conversation: a new tab in the chosen terminal resumes it."""
        folder=s.get('cwd') or str(HOME);command=RESUME[s['tool']]%s['id']
        line=f'cd {shlex.quote(folder)} && {command}'
        if terminal=='Ghostty':
            applescript(f'''tell application "Ghostty"
  activate
  set c to new surface configuration
  set initial working directory of c to {quoted(folder)}
  set initial input of c to {quoted(command+chr(10))}
  if (count of windows) > 0 then
    new tab in front window with configuration c
  else
    new window with configuration c
  end if
end tell''')
        elif terminal=='Terminal':applescript(f'tell application "Terminal"\n  activate\n  do script {quoted(line)}\nend tell')
        elif terminal=='iTerm':
            applescript(f'''tell application "iTerm"
  activate
  set w to (create window with default profile)
  tell current session of w to write text {quoted(line)}
end tell''')
        else:
            subprocess.run(['open','warp://action/new_tab?path='+folder],timeout=8)
            if self.typist is None:return '已在 Warp 打开文件夹'
            time.sleep(1.5);self.typist(line,True)
        return f'已在 {terminal} 继续对话'

    def web(self):
        """The assistant page's AI 控制器 section: the settings, which terminals are installed, the conversations."""
        found=self.scan()
        installed=lambda name:any(Path(folder,name+'.app').exists() for folder in ('/Applications','/System/Applications/Utilities',HOME/'Applications'))
        return {'config':self.config(),'terminals':[{'name':t,'installed':installed(t)} for t in TERMINALS],
                'sessions':[{'tool':t,'id':s['id'],'title':plain(s['title'],60) or '未命名对话','folder':Path(s.get('cwd') or '').name,'state':s['code']}
                            for t in TOOLS for s in found[t][:8]]}
