"""语音记账 on the Mac: a fake badge hands entries over in batches; the CSV keeps one row per entry."""
import sys
import tempfile
import time
from pathlib import Path
from unittest import TestCase

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_ledger import Ledger


class Badge:
    """The badge's side of /ledger: entries until acknowledged by id and revision, six at a time."""
    def __init__(self,entries):self.entries={e['id']:e for e in entries};self.month=None;self.asks=0
    def request(self,path,data):
        assert path=='/ledger';self.asks+=1;self.month=data['month']
        for identity,revision in data['ack']:
            if identity in self.entries and self.entries[identity]['rev']==revision:del self.entries[identity]
        batch=list(self.entries.values())[:6]
        return {'entries':batch,'left':len(self.entries)-len(batch)}


class LedgerTests(TestCase):
    def test_batches_changes_deletions_and_month(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'记账.csv';now=int(time.time())
            entries=[{'id':now+i,'rev':0,'at':now,'cents':1000+i,'cat':'餐饮','note':'午饭%d'%i} for i in range(8)]
            entries.append({'id':now+99,'rev':0,'at':now,'cents':20000,'cat':'收入','note':'收到红包'})
            badge=Badge(entries);ledger=Ledger(path);ledger.sync(badge.request)
            self.assertFalse(badge.entries);self.assertEqual(len(ledger.rows),9)
            self.assertEqual(badge.month[:2],[sum(1000+i for i in range(8)),20000])
            # A changed entry replaces its row; a deleted one removes it; the file reads back the same.
            badge.entries={now:{'id':now,'rev':1,'at':now,'cents':3500,'cat':'交通','note':'打车'},now+1:{'id':now+1,'rev':1,'del':1}}
            ledger.sync(badge.request);self.assertFalse(badge.entries)
            again=Ledger(path);self.assertEqual(len(again.rows),8)
            self.assertEqual((again.rows[now]['分类'],again.rows[now]['金额'],again.rows[now]['类型']),('交通','35.00','支出'))
            self.assertEqual(again.rows[now+99]['类型'],'收入')
            self.assertTrue(path.read_bytes().startswith(b'\xef\xbb\xbf'),'Excel needs the BOM')

    def test_due_only_when_waiting_or_month_unknown(self):
        with tempfile.TemporaryDirectory() as folder:
            ledger=Ledger(Path(folder)/'x.csv')
            self.assertFalse(ledger.due({'ledger':[0,1]}));self.assertFalse(ledger.due({}))
            self.assertTrue(ledger.due({'ledger':[2,1]}));self.assertTrue(ledger.due({'ledger':[0,0]}))
            ledger.last=time.monotonic();self.assertFalse(ledger.due({'ledger':[2,1]}),'not twice within 5 s')
