"""Service contracts without a microphone, account RPC or network calls."""
import base64,json,sys,time,threading
from pathlib import Path
from types import SimpleNamespace as NS
from unittest import TestCase
from unittest.mock import patch,Mock,MagicMock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_services import Services,CodexReader,cursor_usage,gemini_usage,claude_usage
import passport_services
from urllib.error import HTTPError

class ServiceTests(TestCase):
    def test_claude_429_keeps_last_good_windows_and_retries_later(self):
        # ponytail: 429 from the shared usage endpoint must not blank the card; the 5-minute retry is the only backoff.
        good={'windows':[{'usedPercent':40,'windowDurationMins':300,'resetsAt':1}]}
        limited=HTTPError('url',429,'Too Many Requests',None,None)
        service=Services.__new__(Services);service.claude=(0,None,0);service.details={}
        clock=[1000.0]
        with patch('passport_services.time.monotonic',side_effect=lambda:clock[0]),patch.object(passport_services,'CLAUDE_LIMITS',Path('/nonexistent')):
            with patch.object(passport_services,'claude_usage',return_value=good) as read:
                self.assertEqual(service.claude_snapshot(),good['windows']);read.assert_called_once()
            clock[0]+=61
            with patch.object(passport_services,'claude_usage',side_effect=limited) as read:
                self.assertEqual(service.claude_snapshot(),good['windows'])  # kept, not blanked
                clock[0]+=60; self.assertEqual(service.claude_snapshot(),good['windows']); read.assert_called_once()  # within 5 min: no retry
                clock[0]+=240; self.assertEqual(service.claude_snapshot(),good['windows']); self.assertEqual(read.call_count,2)
                clock[0]+=1900; self.assertEqual(service.claude_snapshot(),[])  # last good is over 30 minutes old: blank
    def test_claude_prefers_fresh_status_line_limits_over_the_endpoint(self):
        import tempfile
        service=Services.__new__(Services);service.claude=(0,None,0);service.details={}
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'limits.json'
            path.write_text(json.dumps({'five_hour':{'used_percentage':9,'resets_at':1793709600.0},'saved':time.time()}))
            with patch.object(passport_services,'CLAUDE_LIMITS',path),patch.object(passport_services,'claude_usage') as read:
                self.assertEqual(service.claude_snapshot(),[{'usedPercent':9,'windowDurationMins':300,'resetsAt':1793709600}])
                read.assert_not_called()
                path.write_text(json.dumps({'five_hour':{'used_percentage':9,'resets_at':1},'saved':time.time()-601}))
                read.return_value={'windows':[]}; service.claude_snapshot(); read.assert_called_once()  # stale: endpoint
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
        service=Services.__new__(Services);service.details={};service.seen_completions=set();service.claude_snapshot=lambda:[];service.usage_snapshot=lambda *a:[]
        service.codex=NS(snapshot=lambda:{'windows':[{'usedPercent':11,'windowDurationMins':10080,'resetsAt':123}], 'tasks':[{'id':'one','event':'task_complete','event_id':'event','title':'PRIVATE_TASK_TITLE','event_at':'now'}]})
        data=service.codex_snapshot()
        self.assertEqual(data['windows'][0],{'used':11,'minutes':10080,'resets':123})  # empty slots follow;self.assertNotIn('PRIVATE_TASK_TITLE',str(data));self.assertEqual(data['notice'],'任务已完成')
        self.assertEqual(service.codex_snapshot()['notice'],'')

    def test_cursor_usage_reads_the_current_period(self):
        payload=json.dumps({'billingCycleStart':'1793615007943','billingCycleEnd':'1794219807943',
                            'planUsage':{'totalPercentUsed':5.027,'apiPercentUsed':0,'autoPercentUsed':5.027}}).encode()
        token='h.'+base64.urlsafe_b64encode(json.dumps({'sub':'user_01ABC'}).encode()).decode().rstrip('=')+'.s'
        class Open:
            def __init__(self,body):self.body=body
            def __enter__(self):return self
            def __exit__(self,*a):return None
            def read(self,n):return self.body
            def execute(self,*a):return NS(fetchone=lambda:(token,))
        response=Open(payload);db=Open(None)
        with patch('passport_services.urllib.request.urlopen',return_value=response) as opened,patch('sqlite3.connect',return_value=db):
            data=cursor_usage()
        self.assertEqual([w['usedPercent'] for w in data['windows']],[5.027,0])
        self.assertEqual(data['windows'][0]['windowDurationMins'],10080)
        self.assertEqual(data['windows'][0]['resetsAt'],1794219807)
        request=opened.call_args.args[0]
        self.assertTrue(request.full_url.endswith('/api/dashboard/get-current-period-usage'))
        self.assertEqual(request.data,b'{}')

    def test_gemini_usage_keeps_the_most_used_gemini_and_third_party_model(self):
        envelope='go-keyring-base64:'+base64.b64encode(json.dumps({'token':{'access_token':'ya29.test'}}).encode()).decode()
        models={'gemini-2.5-pro':{'quotaInfo':{'remainingFraction':0.75,'resetTime':'2026-11-03T13:21:07Z'}},
                'gemini-3-flash':{'quotaInfo':{'remainingFraction':0.9,'resetTime':'2026-11-03T13:21:07Z'}},
                'gemini-3.1-flash-image':{'quotaInfo':{'remainingFraction':0.1,'resetTime':'2026-11-03T13:21:07Z'}},
                'claude-opus-5-5-high':{'quotaInfo':{'remainingFraction':0.4,'resetTime':'2026-11-03T14:00:00Z'}},
                'gpt-oss-120b-medium':{'quotaInfo':{'remainingFraction':1,'resetTime':'2026-11-03T14:00:00Z'}},
                'chat_20706':{'quotaInfo':{'remainingFraction':0}}}
        response=MagicMock();response.read.return_value=json.dumps({'models':models}).encode()
        response.__enter__.return_value=response
        with patch('passport_services.subprocess.run',return_value=NS(stdout=envelope+'\n')) as keychain,\
             patch('passport_services.urllib.request.urlopen',return_value=response) as opened:
            data=gemini_usage()
        self.assertEqual(keychain.call_args.args[0][:5],['security','find-generic-password','-s','gemini','-a'])
        self.assertEqual(data['windows'][0]['usedPercent'],25.0)  # Gemini 4 / Pro: 25% used
        self.assertEqual(data['windows'][1]['usedPercent'],10.0)  # Flash: 10% used
        self.assertEqual(data['windows'][2]['usedPercent'],60.0)  # Third-party: Claude Opus at 40% left -> 60% used
        self.assertEqual(data['windows'][2]['resetsAt'],1793714400)
        self.assertEqual(opened.call_args.args[0].get_header('Authorization'),'Bearer ya29.test')

    def test_gemini_usage_renews_an_expired_token_through_agy(self):
        wrap=lambda token,expiry:NS(stdout='go-keyring-base64:'+base64.b64encode(json.dumps({'token':{'access_token':token,'expiry':expiry}}).encode()).decode())
        response=MagicMock();response.read.return_value=b'{"models":{}}';response.__enter__.return_value=response
        with patch('passport_services.subprocess.run',side_effect=[wrap('old','2020-01-01T00:00:00+08:00'),NS(stdout=''),wrap('new','2099-01-01T00:00:00+08:00')]) as run,\
             patch('passport_services.urllib.request.urlopen',return_value=response) as opened:
            gemini_usage()
        self.assertEqual(run.call_args_list[1].args[0][1:],['models'])
        self.assertEqual(opened.call_args.args[0].get_header('Authorization'),'Bearer new')

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
