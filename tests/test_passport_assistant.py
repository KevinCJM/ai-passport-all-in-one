import json
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace as Port
from unittest import TestCase
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import passport_assistant as app

MAC='10:20:30:40:50:60'
CONFIG={'mac':MAC,'api_token':'a'*64,'http_forward':99,'wifi_pass':'DO_NOT_COPY','backup':'DO_NOT_COPY'}


class AssistantTests(TestCase):
    def test_paired_device_remains_visible_unplugged_and_usb_deduplicates(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());app.save_config(controller.path,CONFIG)
            controller.device=Port(transport='ble',last_seen=0,close=Mock())
            controller.step([]);choices=controller.snapshot()['devices']
            self.assertEqual(choices,[{'mac':MAC,'port':None,'paired':True,'transport':'蓝牙'}])
            own=Port(device='/dev/original',vid=0x303a,pid=0x1001,serial_number=MAC)
            other=Port(device='/dev/excluded',vid=0x303a,pid=0x4001,serial_number='123456')
            controller.step([own,other]);choices=controller.snapshot()['devices']
            self.assertEqual(len(choices),1);self.assertEqual(choices[0]['port'],'/dev/original');self.assertTrue(choices[0]['paired'])

    def test_bluetooth_permission_is_independent_of_keyboard_permission(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());controller.deps={'accessibility':True,'post_event_access':True,'bluetooth_denied':True}
            self.assertEqual(controller.permission_event(),'required');self.assertIsNone(controller.permission_event())
            controller.deps['bluetooth_denied']=False
            self.assertEqual(controller.permission_event(),'granted')

    def test_bluetooth_authorization_states_do_not_use_system_pairing(self):
        for code,allowed,denied in [(0,False,False),(1,False,True),(2,False,True),(3,True,False)]:
            with patch.dict(sys.modules,{'CoreBluetooth':Port(CBManager=Port(authorization=lambda:code))}),patch.object(app,'codex_command'),patch.object(app.subprocess,'run',return_value=Port(stdout=b'{}')):
                result=app.dependencies()
            self.assertEqual(result['bluetooth_allowed'],allowed);self.assertEqual(result['bluetooth_denied'],denied)

    def test_permission_guide_waits_for_check_prompts_once_and_detects_grant(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock())
            self.assertIsNone(controller.permission_event())
            controller.deps={'accessibility':False,'post_event_access':False}
            self.assertEqual(controller.permission_event(),'required');self.assertTrue(controller.permission_polling.is_set())
            self.assertIsNone(controller.permission_event())
            controller.deps={'accessibility':True,'post_event_access':False}
            self.assertIsNone(controller.permission_event());self.assertFalse(controller.permission_refresh.is_set())
            controller.deps={'accessibility':True,'post_event_access':True}
            self.assertEqual(controller.permission_event(),'granted');self.assertFalse(controller.permission_polling.is_set());self.assertTrue(controller.permission_refresh.is_set())
            self.assertIsNone(controller.permission_event())
            controller.deps={'accessibility':False,'post_event_access':False}
            self.assertEqual(controller.permission_event(),'required')

    def test_already_authorized_launch_is_silent_and_refresh_never_starts_voice(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());controller.deps={'accessibility':True,'post_event_access':True}
            self.assertIsNone(controller.permission_event());self.assertFalse(controller.permission_refresh.is_set())
            for state,expected in [({'link_ready':True,'app':2,'dictation_phase':0},1),({'link_ready':False,'app':2},0),({'link_ready':True,'app':5},0),({'link_ready':True,'app':2,'dictation_phase':2},0)]:
                device=Port(snapshot=Mock(return_value=state),command=Mock());controller.device=device;controller.permission_refresh.set();controller.refresh_after_permission()
                self.assertEqual(device.command.call_count,expected)
                if expected:device.command.assert_called_once_with('refresh_service')

    def test_temporary_permission_probe_failure_never_becomes_denied_or_prompts(self):
        with patch.object(app,'codex_command'),patch.object(app.subprocess,'run',side_effect=TimeoutError()):
            unknown=app.dependencies()
        self.assertIsNone(unknown['accessibility']);self.assertIsNone(unknown['post_event_access'])
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());granted={'accessibility':True,'post_event_access':True}
            controller.deps=granted;self.assertIsNone(controller.permission_event())
            controller.deps=unknown;self.assertIsNone(controller.permission_event())
            self.assertTrue(controller.permission_state);self.assertTrue(controller.permission_polling.is_set())
            controller.deps=granted;self.assertIsNone(controller.permission_event())
            self.assertFalse(controller.permission_polling.is_set())

    def test_permission_refresh_retries_transient_link_without_repeating_prompt(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());controller.permission_refresh.set()
            state={'link_ready':False,'app':2,'dictation_phase':0}
            controller.device=Port(snapshot=lambda:state,command=Mock(side_effect=[ConnectionError(),{'result':0}]))
            controller.refresh_after_permission();controller.device.command.assert_not_called();self.assertTrue(controller.permission_refresh.is_set())
            state['link_ready']=True;controller.refresh_after_permission();self.assertTrue(controller.permission_refresh.is_set())
            controller.refresh_after_permission();self.assertFalse(controller.permission_refresh.is_set());self.assertEqual(controller.device.command.call_count,2)

    def test_import_only_keeps_device_credentials_and_fixed_local_ports(self):
        with tempfile.TemporaryDirectory() as folder:
            target=Path(folder)/'device.json';app.save_config(target,CONFIG)
            saved=json.loads(target.read_text());self.assertNotIn('wifi_pass',saved);self.assertNotIn('backup',saved)
            self.assertEqual(saved['http_forward'],18763);self.assertEqual(target.stat().st_mode&0o777,0o600)

    def test_missing_or_invalid_credentials_rejected(self):
        for bad in ({},{'mac':MAC,'api_token':'bad'},dict(CONFIG,mac='123456')):
            with self.assertRaises(ValueError):app.validate_config(bad)

    def test_no_device_and_unrelated_new_board_never_open_any_port(self):
        with tempfile.TemporaryDirectory() as folder:
            factory=Mock();controller=app.Controller(folder,factory)
            controller.step([]);self.assertIn('配对',controller.status)
            app.save_config(controller.path,CONFIG)
            controller.device=Port(transport='ble',last_seen=time.monotonic(),close=Mock())
            controller.step([Port(device='/dev/cu.usbmodem1234561',vid=0x303a,pid=0x4001,serial_number='123456')])
            self.assertEqual(controller.candidates,[]);self.assertIn('蓝牙',controller.status);factory.assert_not_called()

    def test_unplug_keeps_paired_device_for_bluetooth_reconnect(self):
        with tempfile.TemporaryDirectory() as folder:
            controller=app.Controller(folder,Mock());app.save_config(controller.path,CONFIG)
            own=Port(transport='ble',last_seen=0,close=Mock());controller.device=own;controller.step([]);own.close.assert_not_called()
            self.assertIs(controller.device,own);self.assertFalse(controller.stop.is_set());self.assertIn('蓝牙',controller.status)

    def test_missing_official_apps_only_return_detection_state(self):
        with patch.object(app,'codex_command',side_effect=FileNotFoundError),patch.object(app.subprocess,'run',side_effect=OSError):
            result=app.dependencies()
        self.assertFalse(result['codex'])
