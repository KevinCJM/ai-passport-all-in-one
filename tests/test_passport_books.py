"""电子书 on the computer: chunking, tidying, and sync keeps the badge's window full around the place read."""
import json
import sys
import tempfile
from pathlib import Path
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from passport_books import Books, CHUNK, HEAD, KEEP, decode, tidy, pack, parts


class FakeBadge:
    def __init__(self, slots):
        self.slots = slots; self.sectors = [b''] * slots; self.heads = [None] * slots
        self.meta = {'id': 0, 'size': 0, 'chunks': 0, 'lo': 0, 'hi': 0, 'place': 0}; self.largest = 0

    def request(self, path, data):
        assert path == '/book'
        self.largest = max(self.largest, len(json.dumps({'token': 'x' * 64, 'path': path, 'data': data}, ensure_ascii=False).encode()))
        m = self.meta
        if 'open' in data:
            o = data['open']
            m.update(id=o['id'], size=o['size'], chunks=o['chunks'], lo=o['chunk'], hi=o['chunk'], place=o['place'])
        elif 'drop' in data:
            m['lo'] = min(max(m['lo'], data['drop']), m['hi'])
        elif 'put' in data:
            p = data['put']
            assert p['chunk'] == m['hi'] and m['hi'] - m['lo'] < self.slots
            sector = p['chunk'] % self.slots; raw = p['text'].encode('utf-8')
            if p['at'] == 0: self.sectors[sector] = b''; self.heads[sector] = None
            assert len(self.sectors[sector]) == p['at'] and HEAD + p['at'] + len(raw) <= CHUNK
            self.sectors[sector] += raw
            if 'count' in p:
                self.heads[sector] = (m['id'], p['chunk'], p['first'], p['count'])
                m['hi'] += 1
        return {'id': m['id'], 'place': m['place'], 'lo': m['lo'], 'hi': m['hi'], 'slots': self.slots}

    def status(self):
        return {'book': [self.meta['id'], self.meta['place'], self.meta['lo'], self.meta['hi']]}

    def text_at(self, offset):
        for c in range(self.meta['lo'], self.meta['hi']):
            head = self.heads[c % self.slots]
            if not head: continue
            identity, chunk, first, count = head
            if first <= offset < first + count:
                sec = self.sectors[c % self.slots]
                return sec[offset - first:].decode('utf-8', errors='ignore')
        return None


class BooksTests(TestCase):
    def test_tidy_and_pack(self):
        raw = '第一章\n\n' + '　　他走进房间，看见桌上放着一封信。“你来了？”她问。He said: hello world.\r\n' * 50
        text = tidy(raw)
        self.assertTrue(text.startswith('　　第一章'))
        chunks = pack(text)
        self.assertGreater(len(chunks), 1)
        reconstructed = b''.join(body for _, _, body in chunks)
        self.assertEqual(reconstructed.decode('utf-8'), text)
        for first, count, body in chunks:
            self.assertLessEqual(len(body), CHUNK - HEAD)
            self.assertEqual(count, len(body))
            pieces = parts(body)
            self.assertEqual(''.join(t for _, t in pieces).encode('utf-8'), body)

    def test_sync_keeps_the_window_full_around_place(self):
        with tempfile.TemporaryDirectory() as folder:
            books = Books(folder); badge = FakeBadge(slots=24)
            novel = ''.join('第%d章\n' % c + '　　雨一直在下，街灯把湿漉漉的石板路照得发亮。\n' * 40 for c in range(150))
            books.add('雨夜.txt', novel.encode('gb18030'))
            identity = books.index['current']; chunks = books.chunks(identity)
            self.assertGreater(len(chunks), 3 * badge.slots)
            sync = lambda: [books.sync(badge.request) for _ in range(500) if (setattr(books, 'last', float('-inf')) or books.due(badge.status()))]
            sync()
            self.assertEqual((badge.meta['lo'], badge.meta['hi']), (0, badge.slots))
            self.assertIsNotNone(badge.text_at(0))
            # Read on far into the window: old chunks are dropped and window refills
            badge.meta['place'] = chunks[20][0] + 3; sync()
            self.assertEqual((badge.meta['lo'], badge.meta['hi']), (20 - KEEP, 20 - KEEP + badge.slots))
            self.assertEqual(books.index['books'][str(identity)]['place'], badge.meta['place'])
