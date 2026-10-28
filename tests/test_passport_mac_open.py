import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_mac_open import MacOpener, cut, find, norm  # noqa: E402


def app(name, *others):
    return (Path(f'/Applications/{name}.app'), others[0] if others else name, {norm(n) for n in (name,) + others})


APPS = [app('Google Chrome'), app('WeChat', '微信'), app('微信双开'), app('企业微信'), app('Codex++'),
        app('Codex++ 管理工具'), app('PyCharm Professional Edition'), app('Calculator', '计算器'), app('Lark', '飞书'),
        app('QQ'), app('QQMusic', 'QQ音乐')]


class FindTest(unittest.TestCase):
    def found(self, spoken, aliases=None):
        a = find(spoken, APPS, aliases)
        return a[0].stem if a else None

    def test_names(self):
        self.assertEqual(self.found('chrome'), 'Google Chrome')
        self.assertEqual(self.found('googlechrome'), 'Google Chrome')
        self.assertEqual(self.found('谷歌浏览器'), 'Google Chrome')
        self.assertEqual(self.found('微信'), 'WeChat')          # not 微信双开 or 企业微信
        self.assertEqual(self.found('企业微信'), '企业微信')
        self.assertEqual(self.found('codex'), 'Codex++')        # not Codex++ 管理工具
        self.assertEqual(self.found('pycharm'), 'PyCharm Professional Edition')
        self.assertEqual(self.found('计算器'), 'Calculator')
        self.assertEqual(self.found('飞书'), 'Lark')
        self.assertEqual(self.found('qq'), 'QQ')
        self.assertEqual(self.found('qq音乐'), 'QQMusic')

    def test_misses(self):
        self.assertIsNone(self.found('派查姆'))
        self.assertIsNone(self.found('q'))                      # one letter is no name
        self.assertIsNone(self.found(''))

    def test_aliases(self):
        self.assertEqual(self.found('派查姆', {'派查姆': 'PyCharm'}), 'PyCharm Professional Edition')
        self.assertEqual(self.found('微信', {'微信': '企业微信'}), '企业微信')   # the user's own win

    def test_user_words(self):
        # A word set on the assistant's page names one program by its path, ahead of every other match.
        chrome = '/Applications/Google Chrome.app'
        self.assertEqual(self.found('浏览器', {'浏览器': chrome}), 'Google Chrome')
        self.assertEqual(self.found('微信', {'微信': '/Applications/企业微信.app'}), '企业微信')
        self.assertIsNone(self.found('浏览器', {'浏览器': '/Applications/Gone.app'}))  # uninstalled since

    def test_page(self):
        import passport_mac_open
        passport_mac_open.index = lambda: APPS
        opened = []
        opener = MacOpener(launch=lambda path: opened.append(path.stem) or True)
        listed = opener.catalog()
        self.assertIn({'path': '/Applications/WeChat.app', 'label': '微信 · WeChat'}, listed)
        self.assertIn({'path': '/Applications/QQ.app', 'label': 'QQ'}, listed)
        self.assertEqual(opener.open_path('/Applications/QQ.app'), '已在 Mac 打开 QQ')
        self.assertEqual(opener.open_path('/etc/passwd'), '没有这个程序')  # only programs it listed
        self.assertEqual(opened, ['QQ'])

    def test_opener(self):
        opened = []
        opener = MacOpener(launch=lambda path: opened.append(path.stem) or True)
        opener.apps = APPS
        self.assertEqual(opener.open('微信'), '已在 Mac 打开 微信')
        self.assertEqual(opened, ['WeChat'])
        opener.apps = APPS
        import passport_mac_open
        passport_mac_open.index = lambda: APPS  # the second look finds nothing new
        self.assertEqual(opener.open('派查姆'), 'Mac 上没找到「派查姆」')
        self.assertEqual([h['app'] for h in opener.history], ['微信', None])

    def test_cut(self):
        self.assertEqual(cut('打开' * 20, 10), '打开打')
        self.assertLessEqual(len(cut('Mac 上没找到「' + '长' * 30 + '」').encode()), 60)


if __name__ == '__main__':
    unittest.main()
