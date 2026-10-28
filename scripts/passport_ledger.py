"""语音记账 on this Mac: the badge's entries, taken over USB or Bluetooth, in one CSV that Numbers or Excel opens.

The badge keeps only today's entries (and older ones until this Mac has them). Each sync asks /ledger for what it has
not handed over yet, saves it, and acknowledges each entry by id and revision on the next ask; a changed entry comes
again with a higher revision and replaces its row, a deleted one removes it. The badge gets this month's totals back.
"""
import csv
import io
import os
import time
from datetime import datetime
from pathlib import Path

COLUMNS = ['日期', '时间', '类型', '分类', '金额', '备注', 'id']


class Ledger:
    def __init__(self, path):
        self.path = Path(path); self.rows = {}; self.last = 0; self.stats = {'synced': 0, 'deleted': 0, 'syncs': 0}
        if self.path.exists():
            with self.path.open(encoding='utf-8-sig', newline='') as source:
                for row in csv.DictReader(source):
                    if row.get('id'): self.rows[int(row['id'])] = row

    def save(self):
        out = io.StringIO(); writer = csv.DictWriter(out, COLUMNS, lineterminator='\n'); writer.writeheader()
        for _, row in sorted(self.rows.items(), key=lambda item: (item[1]['日期'], item[1]['时间'], item[0])): writer.writerow(row)
        temporary = self.path.with_suffix('.new'); self.path.parent.mkdir(parents=True, exist_ok=True)
        with temporary.open('w', encoding='utf-8-sig', newline='') as target:
            target.write(out.getvalue()); target.flush(); os.fsync(target.fileno())
        temporary.replace(self.path)

    def apply(self, entries):
        """Saves a batch from the badge; returns the acknowledgements [id, rev] for the next ask."""
        acks = []
        for entry in entries:
            identity, revision = int(entry['id']), int(entry['rev'])
            if entry.get('del'):
                self.stats['deleted'] += self.rows.pop(identity, None) is not None
            else:
                at = datetime.fromtimestamp(int(entry['at'])); cents = int(entry['cents']); income = entry['cat'] == '收入'
                self.rows[identity] = {'日期': at.strftime('%Y-%m-%d'), '时间': at.strftime('%H:%M'), '类型': '收入' if income else '支出',
                                       '分类': entry['cat'], '金额': '%d.%02d' % divmod(cents, 100), '备注': entry.get('note', ''), 'id': str(identity)}
                self.stats['synced'] += 1
            acks.append([identity, revision])
        if acks: self.save()
        return acks

    def month(self):
        """This month's spending and income in cents, and the month as yyyymm."""
        now = datetime.now(); prefix = now.strftime('%Y-%m'); out = income = 0
        for row in self.rows.values():
            if not row['日期'].startswith(prefix): continue
            cents = round(float(row['金额']) * 100)
            if row['类型'] == '收入': income += cents
            else: out += cents
        return [out, income, now.year * 100 + now.month]

    def due(self, state):
        """Whether the badge (its /status) has entries waiting or lacks this month's totals; at most every 5 s."""
        waiting, known = (state.get('ledger') or [0, 1])[:2]
        return (waiting or not known) and time.monotonic() - self.last > 5

    def sync(self, request):
        """Hands entries over until the badge has none left; request(path, data) talks to the badge."""
        self.last = time.monotonic(); self.stats['syncs'] += 1; acks = []
        for _ in range(12):
            reply = request('/ledger', {'ack': acks, 'month': self.month()})
            if 'entries' not in reply: raise RuntimeError('ledger busy')
            acks = self.apply(reply['entries'])
            if not acks: return
        # The last batch still needs its acknowledgement.
        request('/ledger', {'ack': acks, 'month': self.month()})
