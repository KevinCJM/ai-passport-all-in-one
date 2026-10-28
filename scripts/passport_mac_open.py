"""打开 Mac 程序: the badge heard "打开 Mac 的微信" and sends "open:微信"; find that program and open it.

Names come from the bundles themselves (file name, CFBundleName/DisplayName, the Chinese InfoPlist names), plus a
few spoken names the bundles lack (谷歌浏览器 -> Google Chrome) and the user's own words, set on the assistant's page
and kept in settings.json "mac_app_aliases" ({"派查姆": "/Users/.../PyCharm Professional Edition.app"}; a program's
name works as well as its path). What it heard and found is kept in details['mac_open'] to help add words."""
import plistlib
import re
import subprocess
import time
from pathlib import Path

FOLDERS = [Path('/Applications'), Path('/Applications/Utilities'), Path('/System/Applications'),
           Path('/System/Applications/Utilities'), Path.home()/'Applications']
EXTRA = [Path('/System/Library/CoreServices/Finder.app')]
ALIASES = {'谷歌浏览器': 'Google Chrome', '谷歌': 'Google Chrome', '浏览器': 'Google Chrome', '微信': 'WeChat',
           '飞书': 'Lark', '钉钉': 'DingTalk', '腾讯会议': 'TencentMeeting', 'qq音乐': 'QQMusic', '终端': 'Terminal',
           '访达': 'Finder', 'vscode': 'Visual Studio Code', '火狐': 'Firefox', '网易云音乐': 'NeteaseMusic'}


def norm(name):
    return re.sub(r'[\s\-_.+()（）·]', '', str(name)).lower()


def bundles():
    for folder in FOLDERS:
        if not folder.is_dir(): continue
        for item in folder.iterdir():
            if item.suffix == '.app': yield item
            elif item.is_dir():  # DingTalk.localized/DingTalk.app, JetBrains folders
                yield from (inner for inner in item.glob('*.app'))
    yield from (path for path in EXTRA if path.exists())


def chinese_names(app):
    names = []
    resources = app/'Contents/Resources'
    for lproj in ('zh_CN', 'zh-Hans', 'zh_Hans', 'zh-CN'):
        strings = resources/f'{lproj}.lproj/InfoPlist.strings'
        if not strings.exists(): continue
        raw = strings.read_bytes()
        try: names += [v for k, v in plistlib.loads(raw).items() if k in ('CFBundleDisplayName', 'CFBundleName')]
        except Exception:
            for encoding in ('utf-16', 'utf-8'):
                try: text = raw.decode(encoding)
                except UnicodeDecodeError: continue
                names += re.findall(r'"?CFBundle(?:Display)?Name"?\s*=\s*"([^"]+)"', text)
                break
    table = resources/'InfoPlist.loctable'
    if table.exists():
        try:
            data = plistlib.loads(table.read_bytes())
            for lang in ('zh_CN', 'zh-Hans', 'zh_Hans'):
                names += [v for k, v in data.get(lang, {}).items() if k in ('CFBundleDisplayName', 'CFBundleName')]
        except Exception: pass
    return [n for n in names if isinstance(n, str) and n.strip()]


def index():
    """[(path, shown name, {normalised names})] of every program found."""
    found = []
    for app in bundles():
        names = [app.stem]
        try:
            info = plistlib.loads((app/'Contents/Info.plist').read_bytes())
            names += [info.get(k) for k in ('CFBundleDisplayName', 'CFBundleName') if isinstance(info.get(k), str)]
        except Exception: pass
        chinese = chinese_names(app)
        found.append((app, (chinese or names)[0], {norm(n) for n in names + chinese if norm(n)}))
    return found


def find(spoken, apps, aliases=None):
    """The program a spoken name means: an alias, then an exact name, then the shortest name containing it."""
    key = norm(spoken)
    if not key: return None
    mine = {norm(k): v for k, v in (aliases or {}).items()}
    if str(mine.get(key, '')).startswith('/'):  # the user's word for one program
        return next((a for a in apps if str(a[0]) == mine[key]), None)
    target = norm(mine.get(key) or ALIASES.get(key) or key)
    exact = [a for a in apps if target in a[2]]
    if exact: return min(exact, key=lambda a: len(a[1]))
    if len(target) < 2: return None
    partial = [(min(len(n) for n in a[2] if target in n), a) for a in apps if any(target in n for n in a[2])]
    return min(partial, key=lambda p: p[0])[1] if partial else None


def cut(text, limit=60):
    """At most `limit` UTF-8 bytes, whole characters only (the badge's note holds 64)."""
    while len(text.encode()) > limit: text = text[:-1]
    return text


class MacOpener:
    def __init__(self, aliases=lambda: {}, launch=None):
        self.aliases = aliases
        self.launch = launch or (lambda path: subprocess.run(['open', str(path)], capture_output=True, timeout=10).returncode == 0)
        self.apps = None
        self.history = []

    def catalog(self):
        """Every program, freshly looked up, for the page's list: its shown name (with the file's when they differ)."""
        self.apps = index()
        listed = {str(a[0]): a[1] if a[1] == a[0].stem else f'{a[1]} · {a[0].stem}' for a in self.apps}
        return sorted(({'path': p, 'label': l} for p, l in listed.items()), key=lambda a: a['label'].lower())

    def open_path(self, path):
        """The page's 试一下: only a program in the list."""
        app = next((a for a in self.apps or index() if str(a[0]) == path), None)
        if not app: return '没有这个程序'
        return cut(f'已在 Mac 打开 {app[1]}' if self.launch(app[0]) else f'Mac 打不开 {app[1]}')

    def open(self, spoken):
        """The note the badge shows."""
        app = None
        for fresh in (False, True):  # a program installed since the last look: look again once
            if self.apps is None or fresh: self.apps = index()
            app = find(spoken, self.apps, self.aliases())
            if app: break
        ok = bool(app) and self.launch(app[0])
        self.history = (self.history + [{'heard': spoken, 'app': app[1] if app else None, 'ok': ok, 'at': int(time.time())}])[-10:]
        if not app: return cut(f'Mac 上没找到「{spoken}」')
        return cut(f'已在 Mac 打开 {app[1]}' if ok else f'Mac 打不开 {app[1]}')
