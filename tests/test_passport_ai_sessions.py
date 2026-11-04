import json
import os
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
        self.sessions = ai.AISessions({}, lambda: None, self.home/'read.json')

    def write(self, *records):
        with open(self.path, 'a') as f:
            f.write(''.join(records))

    def test_claude_turns(self):
        self.write(line(type='user', cwd='/work/app', timestamp='2026-11-04T01:00:00Z', message={'content': '修一下登录'}))
        self.assertEqual(self.sessions.badge('claude')['rows'], [[0, 0], [0, 1]])   # first run: running, nothing unread
        self.write(line(type='ai-title', aiTitle='修复登录'),
                   line(type='assistant', isSidechain=True, message={'stop_reason': 'end_turn', 'content': [{'type': 'text', 'text': '子任务'}]}),
                   line(type='assistant', timestamp='2026-11-04T01:05:00Z', message={'stop_reason': 'end_turn', 'content': [{'type': 'text', 'text': '**已修好**，见 `login.py`'}]}),
                   '{"type":"user","message":')   # a record still being written
        badge = self.sessions.badge('claude')
        self.assertEqual(badge['rows'], [[0, 0], [1, 0]])
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

    def test_settings(self):
        self.assertEqual(self.sessions.config()['codex'], 'app')
        with self.assertRaises(ValueError):
            self.sessions.configure({'codex': 'app', 'claude': 'terminal', 'terminal': 'Hyper'})
        self.sessions.configure({'codex': 'terminal', 'claude': 'terminal', 'terminal': 'Warp'})
        self.assertEqual(self.sessions.config()['terminal'], 'Warp')


if __name__ == '__main__':
    unittest.main()
