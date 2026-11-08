"""The Windows assistant's own paths, checked on any system: key plans, COM serials, the serial bridge (a pty stands
in for the COM port), the web page's 电脑助手 API without a badge, and Start-menu programs."""
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import passport_windows as win  # noqa: E402
import passport_mac_open as opener  # noqa: E402

# Stands in for passport-ppp --port N: accepts one connection and echoes it.
ECHO = '''import socket,sys
server=socket.socket();server.bind(("127.0.0.1",int(sys.argv[sys.argv.index("--port")+1])));server.listen(1)
connection,_=server.accept()
while data:=connection.recv(4096):connection.sendall(data)'''


class KeyPlanTest(unittest.TestCase):
    def test_actions(self):
        self.assertEqual(win.plan('paste'), [(win.CONTROL, win.V)])
        self.assertEqual(win.plan('enter'), [(win.RETURN,)])
        self.assertEqual(win.plan('delete_line'), [(win.END,), (win.SHIFT, win.HOME), (win.BACK,)])
        self.assertEqual(win.plan('delete_line', terminal=True), [(win.ESCAPE,)])
        self.assertEqual(win.plan('enter', terminal=True), [(win.RETURN,)])


class SerialTest(unittest.TestCase):
    def test_com_port_serials_in_any_format(self):
        import passport_assistant as app
        port = lambda serial, pid=0x1001: SimpleNamespace(vid=0x303a, pid=pid, serial_number=serial, device='COM7')
        found = app.devices([port('34:85:18:AA:BB:CC'), port('348518AABBCD'), port('34-85-18-aa-bb-ce'), port('3485'), port('348518AABBCF', 0x1002)])
        self.assertEqual([d['mac'] for d in found], ['34:85:18:aa:bb:cc', '34:85:18:aa:bb:cd', '34:85:18:aa:bb:ce'])

    @unittest.skipIf(sys.platform == 'win32', 'needs a pty')
    def test_bridge_copies_both_ways_and_ends_with_the_gateway(self):
        import pty
        from passport_computer_link import SerialBridge
        master, slave = pty.openpty()
        import tty;tty.setraw(slave)
        with open(os.devnull, 'w') as log:
            bridge = SerialBridge([sys.executable, '-c', ECHO], os.ttyname(slave), log)
        try:
            os.write(master, b'\x7e\xff\x03hello\x7e')
            received = b''; until = time.monotonic() + 5
            while len(received) < 9 and time.monotonic() < until: received += os.read(master, 64)
            self.assertEqual(received, b'\x7e\xff\x03hello\x7e')
            self.assertIsNone(bridge.poll())
            bridge.process.kill(); bridge.process.wait()
            until = time.monotonic() + 5
            while bridge.poll() is None and time.monotonic() < until: time.sleep(.05)
            self.assertIsNotNone(bridge.poll(), 'gateway gone: the bridge reports it so the device restarts it')
            self.assertFalse(bridge.serial.is_open)
        finally:
            bridge.terminate(); os.close(master); os.close(slave)

    def test_bonded_badge_by_address(self):
        # A bonded badge stops advertising on Windows: reached by its Bluetooth address, MAC + 2 (carry included).
        import passport_device
        badge = passport_device.bonded_passport('AI-Passport-4C11AE31B9FF')
        self.assertEqual((badge.address, badge.details.adv.bluetooth_address), ('4C:11:AE:31:BA:01', 0x4c11ae31ba01))

    def test_gateway_command(self):
        from passport_computer_link import start_gateway
        with patch('passport_computer_link.subprocess.Popen') as popen:
            start_gateway('/dev/cu.usbmodem1', 18763, 19548, None, bridge=False)
        self.assertEqual(popen.call_args.args[0][1:], ['--forward', '18763', '--audio-forward', '19548', '--serial', '/dev/cu.usbmodem1'])
        with patch('passport_computer_link.SerialBridge') as bridge:
            start_gateway('COM7', 18763, 19548, None, bridge=True)
        self.assertEqual(bridge.call_args.args[1], 'COM7')


class AssistantPageTest(unittest.TestCase):
    def test_assistant_api_works_before_the_badge_connects(self):
        spec = importlib.util.spec_from_file_location('windows_device_under_test', ROOT/'scripts/passport_device.py')
        device = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {'passport_services': SimpleNamespace(Services=object), 'passport_device_cloud': SimpleNamespace(DeviceXiaoZhi=object)}):
            spec.loader.exec_module(device)
        assistant = Mock(web_state=Mock(return_value={'status': '还没配对'}), web_action=Mock(side_effect=[{'ok': True}, ValueError('没找到这台胸牌，请检查 USB 线')]))
        server = device.ThreadingHTTPServer(('127.0.0.1', 0), device.Handler); port = server.server_port
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        post = lambda path, body: urllib.request.urlopen(urllib.request.Request(base+path, data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'}))
        with patch.object(device, 'ASSISTANT', assistant), patch.object(device, 'UI_PORT', port):
            thread.start(); base = f'http://127.0.0.1:{port}'
            try:
                with urllib.request.urlopen(base+'/api/assistant') as response: self.assertEqual(json.load(response), {'status': '还没配对'})
                with urllib.request.urlopen(base+'/') as response: self.assertIn('电脑助手'.encode(), response.read())
                with self.assertRaises(urllib.error.HTTPError) as offline: urllib.request.urlopen(base+'/api/state')
                self.assertEqual((offline.exception.code, json.load(offline.exception)), (503, {'error': '胸牌未连接'}))
                with post('/api/assistant', {'action': 'toggle'}) as response: self.assertEqual(json.load(response), {'ok': True})
                with self.assertRaises(urllib.error.HTTPError) as refused: post('/api/assistant', {'action': 'pair', 'mac': 'x'})
                self.assertEqual((refused.exception.code, json.load(refused.exception)['error']), (400, '没找到这台胸牌，请检查 USB 线'))
                with self.assertRaises(urllib.error.HTTPError) as foreign:
                    urllib.request.urlopen(urllib.request.Request(base+'/api/assistant', data=b'{}', headers={'Content-Type': 'application/json', 'Origin': 'https://example.org'}))
                self.assertEqual(foreign.exception.code, 403)
            finally: server.shutdown(); server.server_close(); thread.join(2)

    def test_web_actions(self):
        import passport_assistant as app
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            controller = app.Controller(folder)
            controller.web_action({'action': 'toggle'}); self.assertFalse(controller.enabled)
            controller.web_action({'action': 'location', 'on': True}); self.assertTrue(controller.ip_location_allowed())
            with self.assertRaisesRegex(ValueError, '配对文件无效'): controller.web_action({'action': 'import', 'config': {'mac': 'x'}})
            controller.web_action({'action': 'import', 'config': {'mac': '34:85:18:aa:bb:cc', 'api_token': 'a'*64, 'binary': '/bin/sh'}})
            self.assertEqual(json.loads(controller.path.read_text())['mac'], '34:85:18:aa:bb:cc')
            self.assertNotIn('binary', json.loads(controller.path.read_text()))
            with self.assertRaisesRegex(ValueError, '还没有记账记录'): controller.web_action({'action': 'ledger'})
            with self.assertRaisesRegex(ValueError, '未知操作'): controller.web_action({'action': 'rm'})
            state = controller.web_state()
            self.assertTrue(state['ip_location'] and state['paired'] and state['help'])
            self.assertEqual([row[0] for row in state['features']][:2], ['语音输入', 'Codex'])


class StartMenuTest(unittest.TestCase):
    def test_start_apps_and_aliases_by_app_id(self):
        listing = [{'Name': '微信', 'AppID': r'{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\Tencent\WeChat\WeChat.exe'},
                   {'Name': '计算器', 'AppID': 'Microsoft.WindowsCalculator_8wekyb3d8bbwe!App'},
                   {'Name': '卸载 微信', 'AppID': 'uninstall'}, {'Name': 'Google Chrome', 'AppID': 'Chrome'}]
        result = SimpleNamespace(stdout=json.dumps(listing, ensure_ascii=False).encode('utf-8-sig'))
        with patch.object(opener.subprocess, 'run', return_value=result):
            apps = opener.start_apps()
        self.assertEqual([a[1] for a in apps], ['微信', '计算器', 'Google Chrome'])
        self.assertEqual(opener.find('谷歌浏览器', apps)[0], 'Chrome')
        self.assertEqual(opener.find('浏览器', apps, {'浏览器': 'Microsoft.WindowsCalculator_8wekyb3d8bbwe!App'})[1], '计算器')
        launched = []
        mac = opener.MacOpener(launch=lambda path: launched.append(path) or True)
        with patch.object(opener, 'index', return_value=apps):
            self.assertTrue(mac.open('微信').endswith('打开 微信'))
        self.assertEqual(launched, [listing[0]['AppID']])


if __name__ == '__main__':
    unittest.main()
