"""电子书 on the computer: the tidied text, its chunks, and the sync that keeps the badge's window full around the place.

The fake badge follows BookStore (firmware/passport_suite/main/settings.cpp): a ring of sectors, chunk c in c % slots,
only chunk hi may be written, its header last; it reads text by byte offset."""
import json
import sys
import tempfile
from pathlib import Path
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from passport_books import Books, CHUNK, HEAD, KEEP, pack, parts, tidy


class Badge:
    def __init__(self, slots):
        self.slots = slots; self.sectors = [b''] * slots; self.heads = [None] * slots
        self.meta = {'id': 0, 'size': 0, 'chunks': 0, 'lo': 0, 'hi': 0}; self.place = 0; self.largest = 0

    def request(self, path, data):
        assert path == '/book'
        # Over Bluetooth the whole envelope (token, path, data) must stay under 2304 bytes.
        self.largest = max(self.largest, len(json.dumps({'token': 'x' * 64, 'path': path, 'data': data}, ensure_ascii=False).encode()))
        m = self.meta
        if 'open' in data:
            o = data['open']; m.update(id=o['id'], size=o['size'], chunks=o['chunks'], lo=o['chunk'], hi=o['chunk']); self.place = o['place']
        elif 'drop' in data: m['lo'] = min(max(m['lo'], data['drop']), m['hi'])
        elif 'put' in data:
            p = data['put']; assert p['chunk'] == m['hi'] and m['hi'] - m['lo'] < self.slots
            sector = p['chunk'] % self.slots; raw = p['text'].encode()
            if p['at'] == 0: self.sectors[sector] = b''; self.heads[sector] = None
            assert len(self.sectors[sector]) == p['at'] and HEAD + p['at'] + len(raw) <= CHUNK
            self.sectors[sector] += raw
            if 'count' in p:
                assert p['count'] == len(self.sectors[sector]); self.heads[sector] = (m['id'], p['chunk'], p['first']); m['hi'] += 1
        return {'id': m['id'], 'place': self.place, 'lo': m['lo'], 'hi': m['hi'], 'slots': self.slots}

    def status(self): return {'book': [self.meta['id'], self.place, self.meta['lo'], self.meta['hi']]}

    def text(self, at, size):
        """BookStore::text: the bytes from `at` on, across chunks, as far as the window goes."""
        out = b''
        for c in range(self.meta['lo'], self.meta['hi']):
            identity, chunk, first = self.heads[c % self.slots]; body = self.sectors[c % self.slots]
            assert (identity, chunk) == (self.meta['id'], c)
            if first <= at + len(out) < first + len(body): out += body[at + len(out) - first:]
        return out[:size]


class BooksTests(TestCase):
    def test_tidy_and_chunks(self):
        text = tidy('第一章\r\n\r\n   他走进房间。\t“你来了？”\n\n　　𠀀生僻字\n', set('第一章他走进房间。“你来了？”生僻字 '))
        self.assertEqual(text, '　　第一章\n　　他走进房间。 “你来了？”\n　　□生僻字')
        book = tidy('\n'.join('第%d段。' % i + '雨一直在下，街灯把石板路照得发亮。' * (i % 7 + 1) for i in range(400)))
        chunks = pack(book); raw = book.encode()
        self.assertEqual(b''.join(c[2] for c in chunks), raw)
        for first, length, body in chunks:
            self.assertEqual(raw[first:first + length], body); self.assertLessEqual(length, CHUNK - HEAD)
            body.decode()   # whole characters
        self.assertTrue(all(body.endswith(b'\n') for _, _, body in chunks[:-1]))   # paragraph ends
        for _, _, body in chunks: self.assertEqual(''.join(t for _, t in parts(body)).encode(), body)

    def test_sync_keeps_the_window_full_around_the_place(self):
        with tempfile.TemporaryDirectory() as folder:
            books = Books(folder); badge = Badge(slots=24)
            novel = ''.join('第%d章\n' % c + '　　雨一直在下，街灯把湿漉漉的石板路照得发亮。\n' * 40 for c in range(150))
            books.add('雨夜.txt', novel.encode('gb18030'))
            identity = books.index['current']; chunks = books.chunks(identity)
            raw = (Path(folder) / ('%d.txt' % identity)).read_bytes()
            self.assertGreater(len(chunks), 3 * badge.slots)
            sync = lambda: [books.sync(badge.request) for _ in range(500) if (setattr(books, 'last', float('-inf')) or books.due(badge.status()))]
            sync()
            self.assertEqual((badge.meta['lo'], badge.meta['hi'], badge.meta['size']), (0, badge.slots, len(raw)))
            self.assertEqual(badge.text(100, 600), raw[100:700])
            # Read on far into the window: the read part goes (KEEP chunks stay behind) and the window refills.
            badge.place = chunks[20][0] + 30; sync()
            self.assertEqual((badge.meta['lo'], badge.meta['hi']), (20 - KEEP, 20 - KEEP + badge.slots))
            self.assertEqual(books.index['books'][str(identity)]['place'], badge.place)
            self.assertEqual(badge.text(badge.place, 7000), raw[badge.place:badge.place + 7000])   # across chunk ends too
            # Near the end the window stops at the last chunk.
            badge.place = len(raw) - 10; sync()
            self.assertEqual(badge.meta['hi'], len(chunks)); self.assertEqual(badge.text(len(raw) - 10, 100), raw[-10:])
            self.assertLess(badge.largest, 2304)
            # Another book; back to the first, the badge starts where it was left.
            books.add('短篇.txt', '很短的一本书。'.encode()); sync()
            self.assertEqual(badge.meta['id'], books.index['current']); self.assertEqual(badge.text(0, 100), '　　很短的一本书。'.encode())
            books.choose(identity); sync()
            self.assertEqual((badge.meta['id'], badge.place), (identity, len(raw) - 10))
            self.assertEqual(books.state()['cached'][1], 100.0)
