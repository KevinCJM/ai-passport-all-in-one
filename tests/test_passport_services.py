"""Service contracts without a microphone, account RPC or network calls."""
import sys,time,threading
from pathlib import Path
from types import SimpleNamespace as NS
from unittest import TestCase
from unittest.mock import patch,Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_services import Services,CodexReader

class ServiceTests(TestCase):
    def test_weather_returns_structured_values_for_home_cards(self):
        service=Services.__new__(Services);service.settings={'ip_location_allowed':True}
        service.details={'weather':{'retrieved':time.time(),'location':{'city':'杭州','region':'浙江','country':'中国'},'weather':{'temperature_2m':24.7,'relative_humidity_2m':85,'weather_code':51},'air':{'us_aqi':161},'daily':{'temperature_2m_min':[21],'temperature_2m_max':[31]}}}
        with patch('passport_services.fetch_json') as network:data=service.weather()
        network.assert_not_called()
        self.assertEqual(data['weather'],{'city':'杭州','condition':'小毛毛雨','temperature':24.7,'minimum':21,'maximum':31,'humidity':85,'code':51,'aqi':161})
    def test_voice_input_needs_keyboard_permission(self):
        service=Services.__new__(Services);service.details={}
        for accessibility,posting in [(False,False),(True,False),(False,True)]:
            service.helper=Mock(return_value={'accessibility':accessibility,'post_event_access':posting})
            with self.assertRaises(PermissionError):service.require_dictation_permissions()
            service.helper.assert_called_once_with('status')
        service.helper=Mock(return_value={'accessibility':True,'post_event_access':True})
        service.require_dictation_permissions()

    def test_monitor_has_cpu_memory_swap_and_rates_without_process_names(self):
        service=Services.__new__(Services);service.details={};service.previous_net=(8,NS(bytes_sent=100,bytes_recv=200))
        with patch('passport_services.time.monotonic',return_value=10),patch('passport_services.psutil.cpu_percent',return_value=42.4),patch('passport_services.psutil.virtual_memory',return_value=NS(total=1000,available=300,percent=70)),patch('passport_services.psutil.swap_memory',return_value=NS(used=50)),patch('passport_services.psutil.net_io_counters',return_value=NS(bytes_sent=300,bytes_recv=600)),patch('passport_services.psutil.process_iter') as processes:
            data=service.monitor()
        self.assertLessEqual({'cpu':42,'ram':70,'memory_total':1000,'memory_used':700,'net_up':100,'net_down':200}.items(),data['metrics'].items())
        processes.assert_not_called();self.assertNotIn('top_processes',service.details['monitor'])

    def test_quota_keeps_real_windows_and_omits_task_names(self):
        service=Services.__new__(Services);service.details={};service.seen_completions=set();service.claude_snapshot=lambda:[]
        service.codex=NS(snapshot=lambda:{'windows':[{'usedPercent':11,'windowDurationMins':10080,'resetsAt':123}], 'tasks':[{'id':'one','event':'task_complete','event_id':'event','title':'PRIVATE_TASK_TITLE','event_at':'now'}]})
        data=service.codex_snapshot()
        self.assertEqual(data['windows'][0],{'used':11,'minutes':10080,'resets':123})  # empty slots follow;self.assertNotIn('PRIVATE_TASK_TITLE',str(data));self.assertEqual(data['notice'],'任务已完成')
        self.assertEqual(service.codex_snapshot()['notice'],'')

    def test_codex_cache_avoids_restarting_or_refetching_on_reentry(self):
        reader=CodexReader();reader.start=Mock();reader.rpc=Mock(side_effect=[{'rateLimits':{'primary':{'usedPercent':12,'windowDurationMins':300}}},{'data':[]}])
        first=reader.snapshot();self.assertEqual(reader.snapshot(),first);reader.start.assert_called_once();self.assertEqual(reader.rpc.call_count,2)

    def test_cloud_connection_survives_other_foreground_app(self):
        service=Services.__new__(Services);service.active=(1,1);service.current_request=None
        service.dictation_ble=NS(close=Mock());service.stop_media=Mock()
        service.audio=NS(stop_microphone=Mock(),drain=Mock(),activate=Mock(),callback=None)
        service.sim=NS(physical_device=True);service.codex=NS(close=Mock());service.monitor=Mock(return_value={})
        cloud=NS(suspend=Mock(),resume=Mock(),stats={'stt':'','answer':''},listening=False)
        service.cloud=cloud;service.hello_ready=lambda:True
        service.response({'app':5,'generation':2,'operation':'enter','request':1})
        service.response({'app':1,'generation':3,'operation':'enter','request':2})
        self.assertIs(service.cloud,cloud);self.assertEqual(cloud.resume.call_args.args[0],3);service.codex.close.assert_not_called()
