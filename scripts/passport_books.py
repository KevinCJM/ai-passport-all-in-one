"""电子书 on the computer: the whole txt stays here; the badge keeps a 1 MB window of it and reads from that.

The assistant tidies the text (paragraphs indented by two full-width spaces, blank lines dropped, characters the badge's
font lacks shown as □) and cuts its UTF-8 into 4 KiB chunks, one flash sector each, at paragraph ends where it can. The
badge lays pages out itself, in the font size chosen there, and remembers the byte it is at ("place"). It holds chunks
lo..hi-1 of the current book in a ring of 256 sectors (chunk c in sector c % 256). Each sync drops the chunks read long
ago (KEEP stay behind the place) and appends unread ones until the window is full again, one chunk per call so the poll
loop keeps serving the badge.

Protocol (POST /book over USB or the BLE companion, ≤ 2 KB a request):
  {}                                                -> {"id","place","lo","hi","slots"}
  {"open":{"id","title","size","chunks","chunk","place"}}  a new book (size in bytes): window empty at `chunk`
  {"drop":lo}                                       forget chunks before lo
  {"put":{"chunk","at","text"[,"first","count"]}}   write text at byte `at` of the chunk (0 erases its sector); with
                                                    first/count (its offset in the book, its length) it is complete
"""
import json
import os
import re
import threading
import time
import zlib
from pathlib import Path

CHUNK, HEAD = 4096, 16       # one flash sector; its 16-byte header is written by the badge
KEEP = 16                    # chunks kept behind the one being read (64 KB, about 100 pages)
PART = 1800                  # UTF-8 bytes of JSON-encoded text a request carries (BLE requests are 2304 bytes at most)
INDENT = '　　'


def decode(raw):
    """A txt file's text: UTF-8 (with or without BOM), UTF-16 with BOM, else GB18030, else Big5."""
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'): return raw.decode('utf-16')
    for encoding in ('utf-8-sig', 'gb18030', 'big5'):
        try: return raw.decode(encoding)
        except UnicodeDecodeError: pass
    return raw.decode('utf-8', 'replace')


def tidy(text, chars=None):
    """The text as the badge shows it: one indented paragraph a line, no blank lines, □ for what its font lacks."""
    if chars is None:
        from passport_book_font import CHARS
        chars = set(CHARS)
    paragraphs = (''.join(c if c in chars else ' ' if c == '\t' else '□' for c in line.strip(' \t　﻿') if c >= ' ' or c == '\t')
                  for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'))
    return '\n'.join(INDENT + p for p in paragraphs if p)


def pack(text):
    """The text's UTF-8 in chunks: (offset in the book, length, bytes), each ending at a paragraph end when one falls in
    its last 1 KB, else at a character boundary."""
    raw, chunks, at, room = text.encode(), [], 0, CHUNK - HEAD
    while at < len(raw):
        end = min(len(raw), at + room)
        if end < len(raw):
            cut = raw.rfind(b'\n', end - 1024, end)
            if cut > at: end = cut + 1
            else:
                while raw[end] & 0xc0 == 0x80: end -= 1
        chunks.append((at, end - at, raw[at:end])); at = end
    return chunks or [(0, 0, b'')]


def parts(body):
    """A chunk's text cut at character boundaries into pieces whose JSON stays under PART bytes: (offset, text)."""
    out, at, piece, size = [], 0, '', 2
    for c in body.decode():
        grows = len(json.dumps(c, ensure_ascii=False).encode()) - 2   # bytes, as sent
        if size + grows > PART:
            out.append((at, piece)); at += len(piece.encode()); piece, size = '', 2
        piece += c; size += grows
    out.append((at, piece))
    return out


class Books:
    def __init__(self, folder):
        self.folder = Path(folder); self.lock = threading.Lock(); self.cache = {}; self.last = float('-inf'); self.badge = {}
        self.stats = {'chunks': 0, 'error': ''}
        try: self.index = json.loads((self.folder / 'books.json').read_text(encoding='utf-8'))
        except (OSError, ValueError): self.index = {'current': 0, 'books': {}}
        # Entries from before the badge laid pages out itself (no byte size) are left out.
        self.index['books'] = {k: v for k, v in self.index['books'].items() if 'size' in v}
        if str(self.index['current']) not in self.index['books']: self.index['current'] = 0

    def save(self):
        self.folder.mkdir(parents=True, exist_ok=True); target = self.folder / 'books.json'
        temporary = target.with_suffix('.new'); temporary.write_text(json.dumps(self.index, ensure_ascii=False, indent=1), encoding='utf-8')
        os.replace(temporary, target)

    def add(self, name, raw):
        """Saves an uploaded txt (any of the usual encodings), tidied, as UTF-8 and makes it the book on the badge."""
        text = tidy(decode(raw)); title = re.sub(r'\.txt$', '', Path(name).name, flags=re.I)[:24] or '未命名'
        if not text: raise ValueError('文件是空的')
        identity = zlib.crc32(text.encode()) or 1
        with self.lock:
            self.folder.mkdir(parents=True, exist_ok=True); (self.folder / ('%d.txt' % identity)).write_text(text, encoding='utf-8')
            known = self.index['books'].get(str(identity), {})
            self.index['books'][str(identity)] = {'title': title, 'size': len(text.encode()), 'place': known.get('place', 0), 'added': time.time()}
            self.index['current'] = identity; self.save()
        return self.state()

    def chunks(self, identity):
        if identity not in self.cache:
            self.cache = {identity: pack((self.folder / ('%d.txt' % identity)).read_text(encoding='utf-8'))}
        return self.cache[identity]

    def choose(self, identity):
        with self.lock:
            if str(identity) not in self.index['books']: raise ValueError('没有这本书')
            self.index['current'] = int(identity); self.save()
        return self.state()

    def remove(self, identity):
        with self.lock:
            self.index['books'].pop(str(identity), None)
            if self.index['current'] == int(identity): self.index['current'] = 0
            (self.folder / ('%d.txt' % int(identity))).unlink(missing_ok=True); self.cache.pop(int(identity), None); self.save()
        return self.state()

    def state(self):
        """The shelf, and how much of the current book the badge holds, as percentages [from, to], once it has some."""
        with self.lock:
            books = [dict(book, id=int(key)) for key, book in self.index['books'].items()]
            current, badge, cached = self.index['current'], self.badge, None
        if current and badge.get('id') == current and badge['hi'] > badge['lo']:
            chunks = self.chunks(current); size = sum(c[1] for c in chunks) or 1; last = chunks[badge['hi'] - 1]
            cached = [round(chunks[badge['lo']][0] * 100 / size, 1), round((last[0] + last[1]) * 100 / size, 1)]
        return {'current': current, 'books': sorted(books, key=lambda b: -b['added']), 'badge': badge, 'cached': cached, **self.stats}

    def due(self, status):
        """Whether the badge's book (its /status "book": [id, place, lo, hi]) needs a sync; at most once a second."""
        current = self.index['current']
        if not current or time.monotonic() - self.last < 1: return False
        identity, place, lo, hi = (status.get('book') or [0, 0, 0, 0])[:4]
        if identity != current: return True
        chunks = self.chunks(current); keep = max(0, self.chunk_of(chunks, place) - KEEP); slots = self.badge.get('slots', 256)
        return lo < keep or hi < min(len(chunks), keep + slots) or place != self.index['books'][str(current)]['place']

    @staticmethod
    def chunk_of(chunks, place):
        at = 0
        while at + 1 < len(chunks) and chunks[at + 1][0] <= place: at += 1
        return at

    def sync(self, request):
        """One step toward a full window: open the book, record the place read, drop old chunks or send one chunk."""
        self.last = time.monotonic(); current = self.index['current']
        if not current: return
        book = self.index['books'][str(current)]; chunks = self.chunks(current)
        opening = lambda place: request('/book', {'open': {'id': current, 'title': book['title'], 'size': book['size'], 'chunks': len(chunks),
                                                           'chunk': self.chunk_of(chunks, place), 'place': place}})
        badge = request('/book', {})
        if badge['id'] != current: badge = opening(min(book['place'], max(0, book['size'] - 1)))
        elif badge['place'] != book['place']:
            with self.lock: book['place'] = badge['place']; self.save()
        self.badge = badge
        at = self.chunk_of(chunks, badge['place']); keep = max(0, at - KEEP)
        # The place left the window (the book was reopened elsewhere): start again from it.
        if not badge['lo'] <= at <= badge['hi']: badge = self.badge = opening(badge['place'])
        if badge['lo'] < keep: badge = self.badge = request('/book', {'drop': keep})
        if badge['hi'] < min(len(chunks), badge['lo'] + badge['slots']):
            number = badge['hi']; first, count, body = chunks[number]; pieces = parts(body)
            for index, (offset, text) in enumerate(pieces):
                put = {'chunk': number, 'at': offset, 'text': text}
                if index == len(pieces) - 1: put.update(first=first, count=count)
                badge = request('/book', {'put': put})
            self.badge = badge; self.stats['chunks'] += 1


if __name__ == '__main__':
    # Regenerates passport_book_font.py from the badge's generated UI font: python3 passport_books.py ../firmware/passport_suite/main/ui_font.cpp
    import sys
    codes = [int(x) for x in re.search(r'unicode\[\] = \{(.*?)\}', Path(sys.argv[1]).read_text(), re.S).group(1).split(',')]
    chars = ''.join(sorted(chr(32 + code) for code in codes))   # glyph i + 1 is code point 32 + unicode[i]
    Path(__file__).with_name('passport_book_font.py').write_text(
        '# Generated by passport_books.py from firmware/passport_suite/main/ui_font.cpp: the characters the badge can draw.\n'
        'CHARS = %s\n' % json.dumps(chars, ensure_ascii=False), encoding='utf-8')
