"""Locations shared by the development tools and the self-contained Mac app."""
import os
from pathlib import Path
import shutil
import sys

PACKAGED = bool(getattr(sys, 'frozen', False))
ROOT = Path(__file__).resolve().parents[1]
RESOURCES = Path(getattr(sys, '_MEIPASS', ROOT))


def user_data():
    path = Path(os.environ.get('PASSPORT_DATA_DIR') or
                Path.home() / 'Library/Application Support/AI Passport')
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.chmod(0o700)
    return path


def config_file():
    if os.environ.get('PASSPORT_CONFIG'):
        return Path(os.environ['PASSPORT_CONFIG'])
    return user_data() / 'device.json' if PACKAGED else ROOT / 'experiments/passport-device-data/device.json'


def live_data():
    return user_data() / 'live' if PACKAGED or os.environ.get('PASSPORT_DATA_DIR') else ROOT / 'experiments/passport-live-data'


def helper(name):
    return RESOURCES / 'helpers' / name if PACKAGED else ROOT / 'experiments/passport-connectivity-tools/bin' / name


def ffmpeg():
    bundled = helper('ffmpeg')
    if bundled.is_file():
        return str(bundled)
    found = shutil.which('ffmpeg') or ('/opt/usr/bin/ffmpeg' if Path('/opt/usr/bin/ffmpeg').is_file() else None)
    if not found:
        raise FileNotFoundError('媒体解码组件不可用')
    return found


def codex_command():
    # Only discover the user's own installation; never install or bundle Codex.
    candidates = [Path('/Applications/Codex.app/Contents/Resources/codex'),
                  Path.home() / 'Applications/Codex.app/Contents/Resources/codex',
                  Path('/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex')]
    found = next((str(p) for p in candidates if p.is_file() and os.access(p, os.X_OK)), None)
    found = found or shutil.which('codex')
    if not found:
        raise FileNotFoundError('请自行安装并登录 Codex')
    return [found]

