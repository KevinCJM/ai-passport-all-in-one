import json
import os
import sqlite3
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import passport_ai_sessions as ai  # noqa: E402


def line(**record):
    return json.dumps(record, ensure_ascii=False)+'\n'


class SessionsTest(unittest.TestCase):
    def setUp(self):
        self.home = Path(tempfile.mkdtemp())
        self.folder = self.home/'.claude'/'projects'/'-work'
        self.folder.mkdir(parents=True)
        self.path = self.folder/'abcdef12-0000.jsonl'
        patch = mock.patch.object(ai, 'HOME', self.home)
        patch.start(); self.addCleanup(patch.stop)
        self.sessions = ai.AISessions(self.home/'read.json')
        for name in ('processes',):   # no real CLIs in tests
            patch = mock.patch.object(self.sessions, name, return_value=[]); patch.start(); self.addCleanup(patch.stop)

    def write(self, *records):
        with open(self.path, 'a') as f:
            f.write(''.join(records))

    def test_claude_turns(self):
        self.write(line(type='user', cwd='/work/app', timestamp='2026-11-04T01:00:00Z', message={'content': '修一下登录'}))
        self.assertEqual(self.sessions.badge('claude')['rows'][:2], [[0, 0], [0, 1]])   # first run: running, nothing unread
        self.write(line(type='ai-title', aiTitle='修复登录'),
                   line(type='assistant', isSidechain=True, message={'stop_reason': 'end_turn', 'content': [{'type': 'text', 'text': '子任务'}]}),
                   line(type='assistant', timestamp='2026-11-04T01:05:00Z', message={'stop_reason': 'end_turn', 'content': [{'type': 'text', 'text': '**已修好**，见 `login.py`'}]}),
                   '{"type":"user","message":')   # a record still being written
        badge = self.sessions.badge('claude')
        self.assertEqual(badge['rows'][:2], [[0, 0], [1, 0]])
        self.assertEqual(badge['list'][0][:4], ['abcdef12', '修复登录', 'app', ai.UNREAD])
        self.assertEqual(badge['list'][0][5], '已修好 ，见 login.py')
        with mock.patch.object(self.sessions, 'show', return_value='已切到 Ghostty'):
            self.assertEqual(self.sessions.open('claude', 'abcdef12'), '已切到 Ghostty')
        self.assertEqual(self.sessions.badge('claude')['list'][0][3], ai.DONE)
        self.write('{"content":"好"}}\n', line(type='user', message={'content': '[Request interrupted by user]'}))
        self.assertEqual(self.sessions.badge('claude')['list'][0][3], ai.IDLE)

    def test_stale_turn_and_old_files(self):
        self.write(line(type='user', message={'content': '跑测试'}))
        old = time.time()-ai.STALE-5
        os.utime(self.path, (old, old))
        self.assertEqual(self.sessions.badge('claude')['list'][0][3], ai.IDLE)
        os.utime(self.path, (old-ai.DAY, old-ai.DAY))
        self.assertEqual(self.sessions.badge('claude')['list'], [])

    def test_codex_records(self):
        s = {}
        for record in ({'type': 'event_msg', 'payload': {'type': 'task_started'}},
                       {'type': 'response_item', 'payload': {'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': '完成了'}]}},
                       {'type': 'event_msg', 'timestamp': '2026-11-04T01:00:00Z', 'payload': {'type': 'task_complete', 'last_agent_message': None}}):
            ai.codex_record(s, record)
        self.assertEqual((s['state'], s['reply']), (ai.DONE, '完成了'))

    def test_long_file_starts_at_a_whole_line(self):
        last = line(type='ai-title', aiTitle='标题')
        with mock.patch.object(ai, 'FIRST_READ', len(last.encode())+3):   # starts 3 bytes before the first line ends
            self.write(line(type='user', message={'content': 'x'*80}), last)
            self.assertEqual([r['type'] for r in ai.Tail(str(self.path)).records()], ['ai-title'])

    def test_where_it_opens(self):
        """Running: its host; else its app; else a new tab of the terminal it ran in last (or the last one used)."""
        self.write(line(type='user', cwd='/work/app', message={'content': '跑一下'}))
        found = self.sessions.scan()
        s = found['claude'][0]
        warp = [('claude', 'claude ' + s['id'], '/elsewhere', 'Warp', 'ttys003')]   # matched by its id (sessions/<pid>.json)
        self.assertTrue(self.sessions.attach(found, warp))
        self.assertEqual((s['host'], self.sessions.where[s['id']], self.sessions.where['_last']), (('Warp', 'ttys003'), 'Warp', 'Warp'))
        with mock.patch.object(self.sessions, 'scan'), mock.patch.object(self.sessions, 'focus') as focus:
            self.assertEqual(self.sessions.show(s, True), '已切到 Warp')
        focus.assert_called_once_with(s, 'Warp', 'ttys003')
        self.sessions.attach(found, [])                                          # it was closed
        with mock.patch.object(self.sessions, 'scan'), mock.patch.object(self.sessions, 'resume', return_value='ok') as resume:
            self.sessions.show(s, True)
        resume.assert_called_once_with(s, 'Warp')
        s['app'] = 'Codex'; s['tool'] = 'codex'
        with mock.patch.object(self.sessions, 'scan'), mock.patch.object(ai.subprocess, 'run') as run:
            self.assertEqual(self.sessions.show(s, True), '已在 Codex 打开')
        run.assert_called_once_with(['open', 'codex://threads/'+s['id']], timeout=8, check=True)
        # A CLI without the id in its arguments: the newest conversation in its folder.
        other = {'tool': 'claude', 'id': 'x', 'cwd': '/work/app', 'host': None}
        self.sessions.attach({'claude': [other]}, [('claude', 'claude', '/work/app', 'Ghostty', 'ttys001')])
        self.assertEqual(other['host'], ('Ghostty', 'ttys001'))

    def test_opencode_and_agy_records(self):
        db_path = self.home/'.local/share/opencode/opencode.db'; db_path.parent.mkdir(parents=True)
        with sqlite3.connect(db_path) as db:
            db.executescript("create table session(id,title,directory,time_updated,parent_id,time_archived);"
                             "create table message(id,session_id,time_created,data);create table part(id,message_id,time_created,data);")
            now = int(time.time()*1000)
            db.execute("insert into session values('ses_1','修路由','/work/vpn',?,null,null)", (now,))
            db.execute("insert into message values('m1','ses_1',1,?)", (json.dumps({'role': 'user'}),))
            db.execute("insert into message values('m2','ses_1',2,?)", (json.dumps({'role': 'assistant', 'time': {'completed': now}}),))
            db.execute("insert into part values('p1','m2',1,?)", (json.dumps({'type': 'text', 'text': '路由已修好'}),))
        s = self.sessions.opencode(time.time())[0]
        self.assertEqual((s['title'], s['state'], s['reply']), ('修路由', ai.DONE, '路由已修好'))
        # agy's steps are protobuf: field 1 { field 2: "答复" }, plus a tool call's JSON.
        text = '已完成'.encode(); inner = b'\x12' + bytes([len(text)]) + text
        self.assertEqual(ai.pb_strings(b'\x0a' + bytes([len(inner)]) + inner + b'\x08\x01\x1a\x02{}'), ['已完成', '{}'])

    def test_codex_app_or_cli(self):
        db_path = self.home/'.codex/state_5.sqlite'; db_path.parent.mkdir(parents=True)
        rollout = self.home/'r.jsonl'; rollout.write_text('')
        with sqlite3.connect(db_path) as db:
            db.execute("create table threads(id,rollout_path,cwd,name,title,source,thread_source,archived,updated_at)")
            for tid, source in (('app-1', 'vscode'), ('cli-1', 'cli')):
                db.execute("insert into threads values(?,?,'/w','',?,?,'user',0,?)", (tid, str(rollout), tid, source, int(time.time())))
        self.assertEqual({s['id']: s['app'] for s in self.sessions.codex(time.time())}, {'app-1': 'Codex', 'cli-1': ''})

if __name__ == '__main__':
    unittest.main()
