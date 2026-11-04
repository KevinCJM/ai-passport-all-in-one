"""AI 控制器: the recent conversations of Codex, Claude Code, agy (Antigravity CLI), Cursor and OpenCode for the badge,
read from their own records on this Mac, and the way back to one of them, wherever it was started: its terminal tab,
its app, or a new tab in the terminal it ran in last that resumes it. Nothing to configure.

Records (all undocumented; what cannot be read is left out):
  Codex        ~/.codex/state_5.sqlite threads (source 'vscode' = the app, 'cli' = the terminal) + each rollout jsonl
  Claude Code  ~/.claude/projects/<folder>/<session>.jsonl (entrypoint 'cli'; 'sdk-*' ones are other programs')
  agy          ~/.gemini/antigravity-cli/conversation_summaries.db + conversations/<id>.db (protobuf steps)
  Cursor       CLI ~/.cursor/chats/<hash>/<id>/meta.json (its messages are encrypted); the app: globalStorage/state.vscdb
  OpenCode     ~/.local/share/opencode/opencode.db (its terminal and its app share it and do not say which made a session)
"""
import json
import os
import re
import shlex
import sqlite3
import subprocess
import threading
import time
from datetime import datetime,timezone
from pathlib import Path

HOME=Path.home()
TOOLS=('codex','claude','agy','cursor','opencode')   # the badge's AI 控制器 rows 0-4
DAY=86400
STALE=900                       # a turn with no new record for 15 minutes was stopped (terminal closed, crash)
FIRST_READ=2<<20                # a long record is read from its last 2 MB the first time (Codex rollouts reach 70 MB)
TERMINALS=('Ghostty','Warp','iTerm','Terminal')   # the default for a resume is the first one installed
CLI={'codex':('codex',),'claude':('claude',),'agy':('agy',),'cursor':('cursor-agent','agent'),'opencode':('opencode',)}
RESUME={'codex':'codex resume %s','claude':'claude --resume %s','agy':'agy --conversation %s',
        'cursor':'cursor-agent --resume %s','opencode':'opencode -s %s'}
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


def readonly(path):
    return sqlite3.connect(f'file:{path}?mode=ro',uri=True,timeout=2)


def installed(app):
    return any(Path(folder,app+'.app').exists() for folder in ('/Applications','/System/Applications/Utilities',HOME/'Applications'))


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
    s['cwd']=s.get('cwd') or d.get('cwd');s['entry']=s.get('entry') or d.get('entrypoint')
    m=d.get('message');m=m if isinstance(m,dict) else {};content=m.get('content')
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


def varint(data,i):
    value=shift=0
    while True:
        if i>=len(data):raise ValueError
        byte=data[i];i+=1;value|=(byte&127)<<shift;shift+=7
        if byte<128:return value,i


def pb_strings(data,depth=0):
    """The text fields of a protobuf message, nested ones included (agy keeps its steps as protobuf)."""
    out=[];i=0
    while i<len(data):
        key,i=varint(data,i);kind=key&7
        if kind==0:_,i=varint(data,i)
        elif kind in (1,5):i+=8 if kind==1 else 4
        elif kind==2:
            size,i=varint(data,i);chunk=data[i:i+size];i+=size
            if len(chunk)<size:raise ValueError
            try:
                text=chunk.decode()
                if all(c>=' ' or c in '\n\t\r' for c in text):out.append(text);continue
            except UnicodeDecodeError:pass
            if depth<10:
                try:out+=pb_strings(chunk,depth+1)
                except ValueError:pass
        else:raise ValueError
    return out


def agy_reply(path):
    """The last thing agy said: the longest text of its last answer step (type 15), tool calls' JSON aside."""
    try:
        with readonly(path) as db:row=db.execute('select step_payload from steps where step_type=15 order by idx desc limit 1').fetchone()
        texts=[t for t in pb_strings(row[0] or b'') if not t.startswith('{')] if row else []
    except (sqlite3.Error,ValueError):return ''
    return max(texts,key=len,default='')


def applescript(source):
    return subprocess.run(['osascript','-e',source],capture_output=True,text=True,timeout=10).stdout.strip()


def quoted(text):
    return '"'+text.replace('\\','\\\\').replace('"','\\"')+'"'


class AISessions:
    def __init__(self,read_path,typist=None):
        self.read_path=read_path;self.where_path=read_path.with_name('ai-where.json');self.typist=typist
        self.tails={};self.found={};self.target=None;self.lock=threading.Lock()   # the badge's requests, its aigo events, watch()
        try:self.read=json.loads(read_path.read_text())
        except (OSError,ValueError):self.read=None   # first run: everything already finished counts as read
        # Where each conversation's CLI was last seen running (a terminal or an editor), and '_last': the last terminal.
        try:self.where=json.loads(self.where_path.read_text())
        except (OSError,ValueError):self.where={}

    def session(self,key,path,parse,base):
        found=self.found.get(key)
        if found is None or found['path']!=path:found=self.found[key]=dict(base,path=path,state=IDLE,at=0)
        found.update(base)
        tail=self.tails.get(path)
        if tail is None:tail=self.tails[path]=Tail(path)
        for record in tail.records():parse(found,record)
        return found

    def keep(self,key,base):
        """A conversation read whole each time (from a database): its entry, updated."""
        found=self.found.setdefault(key,{'path':''});found.update(base);return found

    def claude(self,now):
        out=[]
        for path in (HOME/'.claude'/'projects').glob('*/*.jsonl'):
            try:modified=path.stat().st_mtime
            except OSError:continue
            if now-modified>DAY:continue
            s=self.session('claude:'+path.stem,str(path),claude_record,{'tool':'claude','id':path.stem,'modified':modified})
            if not s.get('asked') and s['state']==IDLE or (s.get('entry') or '').startswith('sdk'):continue   # nothing asked; a program's
            if 'start' not in s:   # the folder it began in, where `claude --resume` finds it (long files are read from the end)
                s['start']=''
                try:
                    with open(path,'rb') as f:head=f.read(1<<16).split(b'\n')
                except OSError:head=[]
                for line in head:
                    try:s['start']=json.loads(line).get('cwd') or ''
                    except (ValueError,AttributeError):continue
                    if s['start']:break
            s['cwd']=s['start'] or s.get('cwd')
            s['title']=s.get('custom') or s.get('auto') or s.get('asked','')
            s['app']='Claude' if s.get('entry')=='claude-desktop' else ''   # ponytail: the desktop app's entrypoint is a guess, none seen yet
            out.append(s)
        return out

    def codex(self,now):
        out=[]
        try:
            with readonly(HOME/'.codex'/'state_5.sqlite') as db:
                rows=db.execute("select id,rollout_path,cwd,coalesce(nullif(name,''),title),source from threads "
                                "where thread_source='user' and archived=0 and updated_at>? order by updated_at desc limit 12",(int(now-DAY),)).fetchall()
        except sqlite3.Error:return out
        for thread,path,cwd,title,source in rows:
            try:modified=os.path.getmtime(path)
            except (OSError,TypeError):continue
            out.append(self.session('codex:'+thread,path,codex_record,{'tool':'codex','id':thread,'cwd':cwd,'modified':modified,
                                                                      'title':(title or '').split('\n')[0],'app':'Codex' if source=='vscode' else ''}))
        return out

    def agy(self,now):
        base=HOME/'.gemini'/'antigravity-cli';out=[]
        since=datetime.fromtimestamp(now-DAY,timezone.utc).strftime('%Y-%m-%d %H:%M:%S')
        try:
            with readonly(base/'conversation_summaries.db') as db:
                rows=db.execute("select conversation_id,title,workspace_uris,status,not_fully_idle,last_modified_time from conversation_summaries "
                                "where parent_conversation_id='' and step_count>0 and last_modified_time>? order by last_modified_time desc limit 12",(since,)).fetchall()
        except sqlite3.Error:return out
        for cid,title,uris,status,busy,modified in rows:
            try:folder=json.loads(uris)[0].removeprefix('file://')
            except (ValueError,IndexError,TypeError,AttributeError):folder=''
            at=stamp(modified);s=self.keep('agy:'+cid,{'tool':'agy','id':cid,'title':title,'cwd':folder,'modified':at,'app':''})
            s['state']=RUN if status.endswith('RUNNING') or str(busy) in ('1','True') else DONE
            if s['state']==DONE and s.get('at')!=at:s['reply']=agy_reply(base/'conversations'/f'{cid}.db')
            s['at']=at;out.append(s)
        return out

    def cursor(self,now):
        out=[]
        for meta in (HOME/'.cursor'/'chats').glob('*/*/meta.json'):
            try:
                m=json.loads(meta.read_text());modified=max(p.stat().st_mtime for p in meta.parent.iterdir())
            except (OSError,ValueError):continue
            if now-modified>DAY or not m.get('hasConversation'):continue
            s=self.keep('cursor:'+meta.parent.name,{'tool':'cursor','id':meta.parent.name,'title':m.get('title',''),'cwd':m.get('cwd',''),'modified':modified,'app':''})
            # ponytail: its messages are encrypted, so busy = written in the last minute, and no reply text
            s['state']=RUN if now-modified<60 else DONE;s['at']=modified;out.append(s)
        try:
            with readonly(HOME/'Library/Application Support/Cursor/User/globalStorage/state.vscdb') as db:
                rows=db.execute("select value from cursorDiskKV where key>'composerData:' and key<'composerData;' and "
                                "json_extract(cast(value as text),'$.lastUpdatedAt')>?",(int((now-DAY)*1000),)).fetchall()
                for (value,) in rows:
                    d=json.loads(value);heads=d.get('fullConversationHeadersOnly') or []
                    if d.get('isDraft') or not heads:continue
                    at=d.get('lastUpdatedAt',0)/1000;cid=d['composerId']
                    s=self.keep('cursor:'+cid,{'tool':'cursor','id':cid,'title':d.get('name') or '','cwd':'','modified':at,'at':at,'app':'Cursor'})
                    s['state']={'generating':RUN,'completed':DONE}.get(d.get('status'),IDLE)
                    last=next((h['bubbleId'] for h in reversed(heads) if h.get('type')==2),None)
                    bubble=last and db.execute('select value from cursorDiskKV where key=?',(f'bubbleId:{cid}:{last}',)).fetchone()
                    s['reply']=json.loads(bubble[0]).get('text','') if bubble else '';out.append(s)
        except (sqlite3.Error,ValueError,KeyError,TypeError):pass
        return out

    def opencode(self,now):
        out=[]
        try:
            with readonly(HOME/'.local/share/opencode/opencode.db') as db:
                rows=db.execute('select id,title,directory,time_updated from session where parent_id is null and time_archived is null '
                                'and time_updated>? order by time_updated desc limit 12',(int((now-DAY)*1000),)).fetchall()
                for sid,title,folder,updated in rows:
                    at=updated/1000
                    # Its terminal and its app share the records: one never seen in a terminal is taken for the app's.
                    app='OpenCode' if self.where.get(sid) not in TERMINALS and installed('OpenCode') else ''
                    s=self.keep('opencode:'+sid,{'tool':'opencode','id':sid,'title':title,'cwd':folder,'modified':at,'app':app})
                    if s.get('at')==at:out.append(s);continue
                    last=db.execute('select id,data from message where session_id=? order by time_created desc limit 1',(sid,)).fetchone()
                    d=json.loads(last[1]) if last else {}
                    if d.get('role')!='assistant':s['state']=RUN if d else IDLE
                    elif d.get('error'):s['state']=IDLE
                    elif (d.get('time') or {}).get('completed'):
                        s['state']=DONE
                        parts=db.execute('select data from part where message_id=? order by time_created',(last[0],)).fetchall()
                        s['reply']='\n'.join(p.get('text','') for p in map(json.loads,(x[0] for x in parts)) if p.get('type')=='text')
                    else:s['state']=RUN
                    s['at']=at;out.append(s)
        except (sqlite3.Error,ValueError,TypeError):pass
        return out

    def scan(self):
        """{tool: its conversations, newest first}, each with its badge state code and, running, where ('host')."""
        with self.lock:return self.scan_locked()

    def scan_locked(self):
        now=time.time();result={tool:getattr(self,tool)(now) for tool in TOOLS}
        first=self.read is None
        if first:self.read={}
        for found in result.values():
            for s in found:
                if s['state']==RUN and now-s['modified']>STALE:s['state']=IDLE
                if first and s['state']==DONE:self.read[s['id']]=now
                s['code']=s['state'] if s['state']!=DONE or self.read.get(s['id'],0)>=s['at'] else UNREAD
            found.sort(key=lambda s:s['at'] or s['modified'],reverse=True)
        if first:self.keep_read()
        if self.attach(result):
            try:self.where_path.write_text(json.dumps(self.where))
            except OSError:pass
        return result

    def keep_read(self):
        live={s['id'] for s in self.found.values()}
        self.read={k:v for k,v in self.read.items() if k in live}
        try:self.read_path.write_text(json.dumps(self.read))
        except OSError:pass

    def watch(self):
        """Every minute: which conversation runs where, so a closed one comes back in the same terminal."""
        while True:
            try:self.scan()
            except Exception:pass   # a record being rewritten; next minute
            time.sleep(60)

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

    def processes(self):
        """The AI CLIs running now: [(tool, args, cwd, the .app hosting it or '', tty)]."""
        table={}
        for line in subprocess.run(['ps','-axo','pid=,ppid=,tty=,args='],capture_output=True,text=True,timeout=5).stdout.splitlines():
            pid,ppid,tty,args=(line.split(None,3)+['','',''])[:4]
            table[pid]=(ppid,tty,args)
        names={name:tool for tool,names in CLI.items() for name in names}
        mine={pid:names[Path(args.split(' ')[0]).name] for pid,(_,_,args) in table.items()
              if Path(args.split(' ')[0]).name in names and '.app/' not in args.split(' ')[0]}
        if not mine:return []
        cwd={}
        out=subprocess.run(['lsof','-a','-d','cwd','-Fn','-p',','.join(mine)],capture_output=True,text=True,timeout=5).stdout
        for pid,folder in re.findall(r'^p(\d+)\n(?:f\w+\n)?n(.+)$',out,re.M):cwd[pid]=folder
        running=[]
        for pid,tool in mine.items():
            hop,hops,app=pid,0,''
            while hop in table and hops<20 and not app:
                program=table[hop][2].split(' ')[0]
                if '.app/Contents/MacOS/' in program:app=program.split('.app/')[0].rsplit('/',1)[-1]
                hop=table[hop][0];hops+=1
            args=table[pid][2]
            if tool=='claude':   # Claude Code names its conversation in ~/.claude/sessions/<pid>.json
                try:args+=' '+json.loads((HOME/'.claude'/'sessions'/f'{pid}.json').read_text())['sessionId']
                except (OSError,ValueError,KeyError,TypeError):pass
            running.append((tool,args,cwd.get(pid,''),app,table[pid][1]))
        return running

    def attach(self,result,running=None):
        """Marks the running conversations with where they run (s['host']); True when that taught something new."""
        running=self.processes() if running is None else running;learned=False
        for found in result.values():
            for s in found:s['host']=None
        for tool,args,cwd,app,tty in running:
            # ponytail: a CLI without the id in its arguments is taken for the newest conversation in its folder
            s=next((s for s in result.get(tool,()) if s['id'] in args),None) or next((s for s in result.get(tool,()) if cwd and s.get('cwd')==cwd and not s['host']),None)
            if s is None:continue
            s['host']=(app,tty)
            if app and self.where.get(s['id'])!=app:self.where[s['id']]=app;learned=True
            if app in TERMINALS and self.where.get('_last')!=app:self.where['_last']=app;learned=True
        return learned

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
        """Where it runs now, else its app, else (resume) a new tab of the terminal it ran in last."""
        if resume:self.scan()   # fresh hosts; 语音输入's refocus keeps the ones found when it was opened
        app,tty=s.get('host') or ('','')
        if app:self.focus(s,app,tty);return '已切到 '+app
        if s.get('app'):return self.open_app(s)
        if not resume:return ''
        return self.resume(s,self.terminal(s))

    def terminal(self,s):
        for name in (self.where.get(s['id']),self.where.get('_last')):
            if name in TERMINALS:return name
        return next((name for name in TERMINALS if installed(name)),'Terminal')

    def open_app(self,s):
        if s['tool']=='codex':subprocess.run(['open','codex://threads/'+s['id']],timeout=8,check=True)
        else:subprocess.run(['open','-a',s['app']],timeout=8)   # ponytail: Cursor/OpenCode/Claude open, not at the conversation (no link known)
        return '已在 '+s['app']+' 打开'

    def focus(self,s,app,tty):
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
        elif app=='Warp' and tty and self.warp(tty,title):pass
        else:
            # Editors and the rest: only windows can be raised, by the title of their selected tab.
            subprocess.run(['open','-a',app],timeout=8)
            if title:applescript(f'''tell application "System Events" to repeat with p in (processes whose bundle identifier is (id of application {quoted(app)}))
  repeat with w in windows of p
    if name of w contains {quoted(title)} then perform action "AXRaise" of w
  end repeat
end repeat''')

    def warp(self,tty,title):
        """Warp has no way to pick a tab from outside, and shows only the focused pane's title as its window's. So the
        conversation's terminal gets a unique title (an escape sequence, nothing printed), and Warp's own keys go
        through its panes (⌘]) and tabs (⇧⌘]) until the window shows it; then its title is put back."""
        marker='⌁'+tty
        try:
            with open('/dev/'+tty,'w') as f:f.write(f'\033]0;{marker}\007')
        except OSError:return False
        time.sleep(.3)
        # ponytail: a pane or tab cycle ends when the title repeats, so two panes titled alike can hide the next ones
        found=applescript(f'''tell application "System Events" to tell (first process whose bundle identifier is "dev.warp.Warp-Stable")
  set frontmost to true
  repeat with w in windows
    if name of w contains "{marker}" then
      perform action "AXRaise" of w
      return "yes"
    end if
  end repeat
  repeat with w in windows
    perform action "AXRaise" of w
    delay 0.15
    set tabStart to name of window 1
    repeat 15 times
      set paneStart to name of window 1
      repeat 6 times
        if name of window 1 contains "{marker}" then return "yes"
        keystroke "]" using command down
        delay 0.12
        if name of window 1 is paneStart then exit repeat
      end repeat
      if name of window 1 contains "{marker}" then return "yes"
      keystroke "]" using {{command down, shift down}}
      delay 0.15
      if name of window 1 is tabStart then exit repeat
    end repeat
  end repeat
  return "no"
end tell''')=='yes'
        try:
            with open('/dev/'+tty,'w') as f:f.write(f'\033]0;{title}\007')   # the CLI sets its own again when it next does
        except OSError:pass
        return found

    def resume(self,s,terminal):
        """No CLI runs the conversation: a new tab in that terminal resumes it."""
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

    def place(self,s):
        """Where the conversation is, for the assistant page."""
        app=(s.get('host') or ('',))[0]
        if app:return app+'（运行中）'
        return s.get('app') or self.terminal(s)

    def web(self):
        """The assistant page's AI 控制器 section: the recent conversations and where each one opens."""
        found=self.scan()
        return {'sessions':[{'tool':t,'id':s['id'],'title':plain(s['title'],60) or '未命名对话','folder':Path(s.get('cwd') or '').name,
                             'state':s['code'],'place':self.place(s)} for t in TOOLS for s in found[t][:5]]}
