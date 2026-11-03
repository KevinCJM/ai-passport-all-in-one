"""飞书提醒: the Feishu/Lark desktop app's banners, read from the macOS notification database.

No Feishu open-platform app, token or admin approval: only what the desktop app already shows as system
notifications (sender and the start of the message), so the assistant needs Full Disk Access to read them.
Nothing is stored: the last messages are kept in memory only.
"""
import plistlib
import sqlite3
import sys
import time
from datetime import datetime
from pathlib import Path

DATABASE = Path.home() / 'Library/Group Containers/group.com.apple.usernoted/db2/db'
BUNDLE = 'com.electron.lark'  # Feishu and Lark share it (com.electron.lark.iron: its meeting helper)
MAC_EPOCH = 978307200  # notification dates count from 2001-01-01
MODES = ('全部提醒', '仅特别关注', '不提醒')
KEEP = 100  # messages kept for the page
SHOWN = 6  # sent to the badge (its 2 KB message)


def words(value):
    """The page's 特别关注 / 不提醒 lists: names or words, split on commas and new lines."""
    if isinstance(value, str): value = value.replace('，', ',').replace('\n', ',').split(',')
    return [w.strip()[:20] for w in value or [] if w.strip()][:40]


def clean(text, most):
    """One line the badge can draw: no characters past U+FFFF (emoji), no line breaks, at most `most` characters."""
    text = ' '.join(''.join(c for c in str(text) if ord(c) <= 0xFFFF).split())
    return text if len(text) <= most else text[:most - 1] + '…'


class Feishu:
    def __init__(self, settings, save, database=DATABASE):
        self.settings = settings; self.save = save; self.database = database
        self.items = []; self.last = None; self.seen = 0; self.error = ''

    def config(self):
        s = self.settings.get('feishu') or {}
        mode = s.get('mode', 0)
        return {'mode': mode if mode in (0, 1, 2) else 0, 'vip': words(s.get('vip')), 'mute': words(s.get('mute'))}

    def configure(self, mode=None, vip=None, mute=None):
        c = self.config()
        if mode is not None:
            if mode not in (0, 1, 2): raise ValueError('mode')
            c['mode'] = mode
        if vip is not None: c['vip'] = words(vip)
        if mute is not None: c['mute'] = words(mute)
        self.settings['feishu'] = c; self.save()
        return c

    def read(self):
        """Feishu's notifications after the last one read (all of them the first time), oldest first."""
        if sys.platform != 'darwin': raise OSError('暂只支持 Mac')
        try:
            with sqlite3.connect(f'file:{self.database}?mode=ro', uri=True, timeout=2) as db:
                rows = db.execute("select r.rec_id,r.data,r.delivered_date from record r join app a on a.app_id=r.app_id "
                                  "where a.identifier=? and r.rec_id>? order by r.rec_id", (BUNDLE, self.last or 0)).fetchall()
        except sqlite3.OperationalError as error:
            # Without Full Disk Access the file cannot be opened at all.
            raise PermissionError('需要完全磁盘访问权限') from error
        found = []
        for rec_id, data, delivered in rows:
            try: req = plistlib.loads(data)['req']
            except Exception: continue
            sender = clean(req.get('titl', ''), 12); text = clean(req.get('body', ''), 48); sub = clean(req.get('subt', ''), 12)
            if sub: text = clean(f'{sub}：{text}', 48)  # a group's banner may name the sender as its subtitle
            if sender or text: found.append({'id': rec_id, 'from': sender or '飞书', 'text': text, 'at': int((delivered or 0) + MAC_EPOCH)})
        if rows: self.last = rows[-1][0]
        return found

    def match(self, item, names):
        return any(w in item['from'] or w in item['text'] for w in names)

    def poll(self):
        """New messages since the last poll; the first poll only fills the list. Returns the one to alert, if any."""
        first = self.last is None
        try: found = self.read(); self.error = ''
        except (OSError, PermissionError) as error: self.error = str(error) or type(error).__name__; return None, False
        if first: self.last = self.last or 0
        c = self.config(); alert = None
        for item in found:
            item['vip'] = self.match(item, c['vip']); item['mute'] = not item['vip'] and self.match(item, c['mute'])
            if not first and c['mode'] != 2 and not item['mute'] and (c['mode'] == 0 or item['vip']): alert = item
        self.items = (self.items + found)[-KEEP:]
        return alert, bool(found) and not first

    def today(self):
        midnight = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        return sum(item['at'] >= midnight for item in self.items)

    def page(self, note=''):
        """What the badge's page shows: the newest SHOWN, newest first; 'n' marks those since the page was last opened."""
        shown = self.items[::-1][:SHOWN]
        return {'mode': self.config()['mode'], 'today': self.today(), 'error': self.error, 'note': note,
                'items': [{'f': i['from'], 't': i['text'], 'at': i['at'], 'v': int(i['vip']), 'n': int(i['id'] > self.seen)} for i in shown]}

    def test(self):
        """A made-up message for the settings page's test button, listed like a real one (until real ones push it out)."""
        item = {'id': (self.items[-1]['id'] if self.items else 0) + .5, 'from': '测试', 'text': '这是一条飞书测试提醒，按确认可以朗读',
                'at': int(time.time()), 'vip': False, 'mute': False}
        self.items = (self.items + [item])[-KEEP:]
        return item

    def opened(self):
        self.seen = self.items[-1]['id'] if self.items else 0

    def state(self):
        """The assistant page's 飞书提醒 section; no message text."""
        return dict(self.config(), error=self.error, today=self.today(), last=self.items[-1]['at'] if self.items else 0,
                    reading=self.last is not None and not self.error)


if __name__ == '__main__':
    # Self-check on a fake notification database (the macOS schema's columns that are read).
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'db'; db = sqlite3.connect(path)
        db.executescript('create table app(app_id integer primary key,identifier text);'
                         'create table record(rec_id integer primary key,app_id integer,data blob,delivered_date real);'
                         "insert into app values(1,'com.electron.lark'),(2,'com.apple.mail');")
        def add(rec, app, title, body, sub=None):
            req = {'titl': title, 'body': body}
            if sub: req['subt'] = sub
            db.execute('insert into record values(?,?,?,?)', (rec, app, plistlib.dumps({'req': req}), time.time() - MAC_EPOCH)); db.commit()
        add(1, 1, '张三', '早上好')
        settings = {}; f = Feishu(settings, lambda: None, path)
        sys.platform = 'darwin'
        assert f.poll() == (None, False) and len(f.items) == 1  # the first poll only fills the list
        add(2, 2, '邮件', '不是飞书'); add(3, 1, '李四', '明天\n十点开会😀')
        alert, new = f.poll(); assert new and alert['from'] == '李四' and alert['text'] == '明天 十点开会' and len(f.items) == 2
        f.configure(mode=1, vip='王五，老板')
        add(4, 1, '项目群', '老板说今天上线', sub='赵六'); add(5, 1, '李四', '收到')
        alert, new = f.poll(); assert alert['from'] == '项目群' and alert['text'] == '赵六：老板说今天上线' and alert['vip']
        f.configure(mode=0, mute=['项目群']); add(6, 1, '项目群', '闲聊')
        assert f.poll()[0] is None  # muted
        f.configure(mode=2); add(7, 1, '老板', '在吗'); assert f.poll()[0] is None  # alerts off
        page = f.page(); assert page['today'] == 6 and len(page['items']) == SHOWN and page['items'][0]['t'] == '在吗' and all(i['n'] for i in page['items'])
        f.opened(); assert not any(i['n'] for i in f.page()['items'])
        f.test(); assert f.page()['items'][0]['f'] == '测试' and f.page()['items'][0]['n']
        add(8, 1, '张三', '新消息'); f.poll(); assert f.page()['items'][0]['t'] == '新消息'  # a real one sorts after the test
        assert len(clean('字' * 80, 48)) == 48 and settings['feishu']['vip'] == ['王五', '老板']
        f.database = Path(folder) / 'missing/db'; assert f.poll() == (None, False) and f.error == '需要完全磁盘访问权限'
    print('passport_feishu self-check ok')
