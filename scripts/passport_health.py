"""健康记录 on this Mac: the badge's records (喝水, 体重, 血糖, 体温, 血压, 经期, 亲密), taken over USB or Bluetooth, in one
CSV that Numbers or Excel opens.

The badge keeps its last 64 records and works every value out itself. Each sync asks /health for what this Mac has not
got yet, saves it, and acknowledges each record by id and revision on the next ask; a changed record (a cup more, a
血糖 time of day changed) comes again with a higher revision and replaces its row, a deleted one removes it.
"""
import csv
import io
import os
import time
from datetime import datetime
from pathlib import Path

from passport_books import decode

COLUMNS = ['日期', '时间', '项目', '数值', '单位', '补充', '标签', '提示', 'id']
NAMES = {'water': '喝水', 'weight': '体重', 'glucose': '血糖', 'temperature': '体温', 'pressure': '血压', 'period': '经期', 'intimacy': '亲密'}
UNITS = {'water': '杯', 'weight': 'kg', 'glucose': 'mmol/L', 'temperature': '℃', 'pressure': 'mmHg'}
FLOWS = ['', '量少', '量中', '量多']


def row(entry):
    """One record as a CSV row."""
    kind = entry['kind']; v1, v2, v3 = (entry.get('v') or [0, 0, 0])[:3]; at = datetime.fromtimestamp(int(entry['at']))
    if kind == 'pressure': value, extra = '%d/%d' % (v1, v2), ('心率 %d' % v3 if v3 else '')
    elif kind == 'water': value, extra = str(v1), '%d ml' % (v1 * v2)
    elif kind == 'period': value, extra = entry.get('tag', ''), FLOWS[v1] if 0 <= v1 < 4 else ''
    elif kind == 'intimacy': value, extra = '1', ''
    else: value, extra = '%d.%d' % divmod(v1, 10), ''
    # 经期 and a 亲密 said with a day keep noon as their time: only the day is known.
    clock = '' if kind == 'period' or (kind == 'intimacy' and at.strftime('%H:%M:%S') == '12:00:00') else at.strftime('%H:%M')
    tag = '' if kind == 'period' else entry.get('tag', '')
    return {'日期': at.strftime('%Y-%m-%d'), '时间': clock, '项目': NAMES.get(kind, kind), '数值': value, '单位': UNITS.get(kind, ''),
            '补充': extra, '标签': tag, '提示': entry.get('flag', ''), 'id': str(int(entry['id']))}


class Health:
    def __init__(self, path):
        self.path = Path(path); self.rows = {}; self.last = -1e9; self.stats = {'synced': 0, 'deleted': 0, 'syncs': 0}
        if self.path.exists():
            # Excel or WPS on Chinese Windows saves an edited CSV as GBK: read that too (it is written back as UTF-8).
            for line in csv.DictReader(io.StringIO(decode(self.path.read_bytes()), newline='')):
                if line.get('id'): self.rows[int(line['id'])] = line

    def save(self):
        out = io.StringIO(); writer = csv.DictWriter(out, COLUMNS, lineterminator='\n'); writer.writeheader()
        for _, line in sorted(self.rows.items(), key=lambda item: (item[1]['日期'], item[1]['时间'], item[0])): writer.writerow(line)
        temporary = self.path.with_suffix('.new'); self.path.parent.mkdir(parents=True, exist_ok=True)
        with temporary.open('w', encoding='utf-8-sig', newline='') as target:
            target.write(out.getvalue()); target.flush(); os.fsync(target.fileno())
        os.chmod(temporary, 0o600)  # health data: this user only
        temporary.replace(self.path)

    def apply(self, entries):
        """Saves a batch from the badge; returns the acknowledgements [id, rev] for the next ask."""
        acks = []
        for entry in entries:
            identity, revision = int(entry['id']), int(entry['rev'])
            if entry.get('del'): self.stats['deleted'] += self.rows.pop(identity, None) is not None
            else: self.rows[identity] = row(entry); self.stats['synced'] += 1
            acks.append([identity, revision])
        if acks: self.save()
        return acks

    def due(self, state):
        """Whether the badge (its /status) has records waiting; at most every 5 s."""
        waiting = (state.get('health') or [0])[0]
        return bool(waiting) and time.monotonic() - self.last > 5

    def sync(self, request):
        """Hands records over until the badge has none left; request(path, data) talks to the badge."""
        self.last = time.monotonic(); self.stats['syncs'] += 1; acks = []
        for _ in range(20):
            reply = request('/health', {'ack': acks})
            if 'entries' not in reply: raise RuntimeError('health busy')
            acks = self.apply(reply['entries'])
            if not acks: return
        request('/health', {'ack': acks})  # the last batch still needs its acknowledgement
