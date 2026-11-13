"""English for the PC Assistant (the badge's 系统设置 > 语言). Text is Chinese in the code and translated as shown:
exact, then "{}" patterns (longest fixed text first), then " · "-joined parts. passport_i18n.json is written by
scripts/gen_i18n.py from firmware/passport_suite/i18n/{portal,assistant}-en.json."""
import json
import locale
import os
import re
from pathlib import Path

import passport_paths

_TABLE = json.loads((Path(__file__).with_name('passport_i18n.json')).read_text(encoding='utf-8'))
_PATTERNS = sorted(((re.compile('^' + '(.*?)'.join(map(re.escape, k.split('{}'))) + '$', re.S), v) for k, v in _TABLE.items() if '{}' in k),
                   key=lambda p: -len(p[0].pattern))
_CJK = re.compile(r'[㐀-鿿　-〿＀-￯]')
_english = None


def _cache():
    return passport_paths.config_file().with_name('language')


def _system_english():
    try:
        from Foundation import NSLocale  # macOS app bundles often have no POSIX locale
        return not str(NSLocale.preferredLanguages()[0]).startswith('zh')
    except Exception:
        name = os.environ.get('LANG') or (locale.getlocale()[0] or '')
        return bool(name) and not name.lower().startswith(('zh', 'chinese'))


def english():
    """The badge's language once seen (remembered for the next start), else the computer's."""
    global _english
    if _english is None:
        try: _english = _cache().read_text().strip() == '1'
        except OSError: _english = _system_english()
    return _english


def follow_badge(language):
    """The badge reported its language (0 Chinese, 1 English); the native window follows on the next start."""
    global _english
    if language not in (0, 1): return
    try:
        path = _cache()
        if not path.exists() or path.read_text().strip() != str(language): path.parent.mkdir(parents=True, exist_ok=True); path.write_text(str(language))
    except OSError: pass
    if _english is None: _english = language == 1


def _one(text):
    if text in _TABLE: return _TABLE[text]
    for pattern, en in _PATTERNS:
        m = pattern.match(text)
        if m:
            pieces = iter(m.groups())
            return re.sub(r'\{\}', lambda _: (lambda p: _TABLE.get(p, p))(next(pieces)), en)
    return None


def T(text):
    if not isinstance(text, str) or not english() or not _CJK.search(text): return text
    core = text.strip(); lead = text[:text.index(core)] if core else ''; trail = text[len(lead) + len(core):]
    out = _one(core)
    if out is None and ' · ' in core:
        parts = [_one(p.strip()) for p in core.split(' · ')]
        if any(p is not None for p in parts): out = ' · '.join(p if p is not None else q for p, q in zip(parts, core.split(' · ')))
    return text if out is None else lead + out + trail


if __name__ == '__main__':  # python3 scripts/passport_i18n.py: a self-check
    _english = True
    assert T('暂停助手') == 'Pause Assistant' and T('胸牌已连接 · 蓝牙') == 'Badge connected · Bluetooth'
    assert T('「微信」 → WeChat（打不开）') == '"微信" → WeChat (won\'t open)' and T('没有这条') == '没有这条'
    assert T('AI Passport 助手 · 正在启动') == 'AI Passport Assistant · Starting'
    print('I18N_PY_OK')
