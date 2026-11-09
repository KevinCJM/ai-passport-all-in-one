"""健康记录 on the Mac: a fake badge hands records over in batches; the CSV keeps one row per record."""
import os
import sys
import tempfile
import time
from pathlib import Path
from unittest import TestCase

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_health import Health


class Badge:
    """The badge's side of /health: records until acknowledged by id and revision, six at a time."""
    def __init__(self,entries):self.entries={e['id']:e for e in entries}
    def request(self,path,data):
        assert path=='/health'
        for identity,revision in data['ack']:
            if identity in self.entries and self.entries[identity]['rev']==revision:del self.entries[identity]
        batch=list(self.entries.values())[:6]
        return {'entries':batch,'left':len(self.entries)-len(batch)}


class HealthTests(TestCase):
    def test_batches_changes_deletions(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'健康.csv';now=int(time.time())
            entries=[{'id':now,'rev':1,'at':now,'kind':'water','v':[3,250,0],'tag':'','flag':''},
                     {'id':now+1,'rev':0,'at':now,'kind':'weight','v':[625,0,0],'tag':'','flag':''},
                     {'id':now+2,'rev':0,'at':now,'kind':'glucose','v':[74,0,0],'tag':'空腹','flag':'偏高'},
                     {'id':now+3,'rev':0,'at':now,'kind':'pressure','v':[120,80,70],'tag':'','flag':''},
                     {'id':now+4,'rev':0,'at':now,'kind':'temperature','v':[378,0,0],'tag':'','flag':'低烧'},
                     {'id':now+5,'rev':0,'at':now,'kind':'period','v':[3,0,0],'tag':'开始','flag':''},
                     {'id':now+6,'rev':0,'at':now,'kind':'intimacy','v':[0,0,0],'tag':'有保护','flag':''}]
            badge=Badge(entries);health=Health(path);health.sync(badge.request)
            self.assertFalse(badge.entries);self.assertEqual(len(health.rows),7)
            r=health.rows
            self.assertEqual((r[now]['数值'],r[now]['补充']),('3','750 ml'))
            self.assertEqual((r[now+1]['数值'],r[now+1]['单位']),('62.5','kg'))
            self.assertEqual((r[now+2]['标签'],r[now+2]['提示']),('空腹','偏高'))
            self.assertEqual((r[now+3]['数值'],r[now+3]['补充']),('120/80','心率 70'))
            self.assertEqual((r[now+5]['数值'],r[now+5]['补充'],r[now+5]['时间']),('开始','量多',''))
            self.assertEqual((r[now+6]['项目'],r[now+6]['标签']),('亲密','有保护'))
            # A changed record replaces its row; a deleted one removes it; the file reads back the same.
            badge.entries={now:{'id':now,'rev':2,'at':now,'kind':'water','v':[4,250,0],'tag':'','flag':''},now+1:{'id':now+1,'rev':1,'del':1}}
            health.sync(badge.request);self.assertFalse(badge.entries)
            again=Health(path);self.assertEqual(len(again.rows),6);self.assertEqual(again.rows[now]['数值'],'4')
            self.assertTrue(path.read_bytes().startswith(b'\xef\xbb\xbf'),'Excel needs the BOM')
            if os.name!='nt':self.assertEqual(path.stat().st_mode&0o777,0o600)  # Windows has no POSIX modes

    def test_due_only_when_waiting(self):
        health=Health(Path(tempfile.mkdtemp())/'健康.csv')
        self.assertFalse(health.due({}));self.assertFalse(health.due({'health':[0]}));self.assertTrue(health.due({'health':[2]}))
        health.last=time.monotonic();self.assertFalse(health.due({'health':[2]}))
