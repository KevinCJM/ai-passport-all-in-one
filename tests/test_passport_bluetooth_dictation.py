"""语音输入 session checks without radio hardware, a cloud connection, keyboard events or the clipboard."""
import json
import struct
import sys
import threading
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock,patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import passport_dictation
from passport_dictation import BluetoothDictation,Recognizer,FRAME


class FakeSocket:
    """XiaoZhi's side: records what is sent and answers the stop with the given text."""
    def __init__(self,text='你好。'):
        self.sent=[];self.text=text;self.closed=False;self.replies=[]
    def send(self,data):
        self.sent.append(data)
        if isinstance(data,str) and json.loads(data).get('state')=='stop':
            self.replies+=[json.dumps({'type':'tts','state':'start'})]+([json.dumps({'type':'stt','text':self.text})] if self.text is not None else [])
    def recv(self,timeout=None):
        if self.replies:return self.replies.pop(0)
        raise TimeoutError
    def close(self):self.closed=True


class RecognizerTests(TestCase):
    def test_audio_before_the_socket_is_ready_is_kept_in_order(self):
        socket=FakeSocket();ready=threading.Event()
        recognizer=Recognizer(connect=lambda:(ready.wait(2),(socket,'s1'))[1])
        recognizer.audio(bytes(FRAME*2+100));recognizer.finish();ready.set()
        self.assertTrue(recognizer.done.wait(3))
        self.assertEqual(recognizer.text,'你好。');self.assertTrue(socket.closed)
        listen=[json.loads(m) for m in socket.sent if isinstance(m,str)]
        self.assertEqual([(m['type'],m['state'],m.get('mode')) for m in listen],[('listen','start','manual'),('listen','stop',None)])
        self.assertTrue(all(m['session_id']=='s1' for m in listen))
        frames=[m for m in socket.sent if isinstance(m,bytes)]
        self.assertEqual(len(frames),3)  # two whole frames and the padded tail
        self.assertIsInstance(socket.sent[0],str);self.assertIsInstance(socket.sent[-1],str)

    def test_no_text_is_an_error_and_cancel_types_nothing(self):
        recognizer=Recognizer(connect=lambda:(FakeSocket(text=None),'s'))
        recognizer.finish();self.assertTrue(recognizer.done.wait(3))
        self.assertIsNone(recognizer.text);self.assertIsInstance(recognizer.error,TimeoutError)
        socket=FakeSocket();cancelled=Recognizer(connect=lambda:(socket,'s'))
        cancelled.audio(bytes(FRAME));cancelled.cancel();self.assertTrue(cancelled.done.wait(3))
        self.assertTrue(cancelled.cancelled);self.assertIsNone(cancelled.error);self.assertTrue(socket.closed)
        self.assertFalse(any(isinstance(m,str) and 'stop' in m for m in socket.sent))


class BluetoothDictationTests(TestCase):
    def setUp(self):
        self.calls=[];self.pushed=[]
        self.services=SimpleNamespace(
            sim=SimpleNamespace(ble=lambda action,**data:self.calls.append((action,data)) or {'ok':True,'result':{}}),
            push=lambda payload,context=None:self.pushed.append(payload))
        self.bridge=BluetoothDictation(self.services);self.bridge.context=(2,7)
        self.recognizer=SimpleNamespace(pcm=[],audio=lambda data:self.recognizer.pcm.append(data),finish=Mock(),cancel=Mock(),
            done=threading.Event(),text=None,error=None,cancelled=False,finished=False)
        patcher=patch('passport_dictation.Recognizer',return_value=self.recognizer);patcher.start();self.addCleanup(patcher.stop)

    def packet(self,kind,sequence,payload=b'',generation=7):
        return {'report':2,'hex':(struct.pack('<BIIHB',kind,generation,11,sequence,len(payload))+payload).ljust(176,b'\0').hex()}

    def start(self):
        self.bridge.handle(self.packet(1,0,struct.pack('<IBB',16000,16,1)))
        self.assertEqual(self.calls[-1][1]['kind'],1)
        self.bridge.handle(self.packet(5,1));self.assertEqual(self.pushed[-1]['value'],'rec')

    def test_utterance_is_recognised_then_pasted_and_shown(self):
        self.start()
        self.bridge.handle(self.packet(2,2,b'\x01\x02'*80))
        self.assertEqual(self.recognizer.pcm,[b'\x01\x02'*80])
        with patch('passport_dictation.paste') as paste:
            self.bridge.handle(self.packet(3,3))
            self.assertEqual(self.calls[-1][1]['kind'],2,'the end is acknowledged at once')
            self.recognizer.finish.assert_called_once();self.assertEqual(self.pushed[-1]['value'],'asr')
            self.recognizer.text=' 明天下午三点开会。 ';self.recognizer.done.set();self.bridge.delivery.join(2)
            paste.assert_called_once();self.assertEqual(paste.call_args.args[0],'明天下午三点开会。')
        done=self.pushed[-1];self.assertEqual((done['value'],done['state']),('done','已输入'))
        self.assertEqual(done['lines'][0],'明天下午三点开会。');self.assertTrue(done['lines'][1].startswith('9 字 · '))
        self.bridge.handle(self.packet(4,4));self.assertIsNone(self.bridge.session)
        self.assertEqual(self.bridge.stats['completed'],1)
        self.assertEqual(self.bridge.status()['lines'][0],'明天下午三点开会。','the page shows the last input again')

    def test_nothing_heard_and_failures_type_nothing(self):
        for text,error,value in [('  ',None,'empty'),(None,TimeoutError(),'fail')]:
            self.recognizer.done.clear();self.recognizer.text=text;self.recognizer.error=error
            self.start()
            with patch('passport_dictation.paste') as paste:
                self.bridge.handle(self.packet(3,2));self.recognizer.done.set();self.bridge.delivery.join(2)
                paste.assert_not_called()
            self.assertEqual(self.pushed[-1]['value'],value);self.bridge.handle(self.packet(4,3))

    def test_device_cancel_drops_the_utterance(self):
        self.start();self.bridge.handle(self.packet(6,2))
        self.recognizer.cancel.assert_called_once();self.assertIsNone(self.bridge.session)
        self.assertEqual(self.pushed[-1]['value'],'ready')

    def test_leaving_after_the_end_still_types_it(self):
        self.start();self.recognizer.finish.side_effect=lambda:setattr(self.recognizer,'finished',True)
        self.bridge.handle(self.packet(3,2));self.bridge.close()
        self.recognizer.cancel.assert_not_called()
        self.recognizer.done.set();self.bridge.delivery.join(2)

    def test_long_text_head_fits_a_device_line(self):
        self.bridge.last=('字'*400,0)
        line=self.bridge.status()['lines'][0]
        self.assertLessEqual(len(line.encode()),passport_dictation.HEAD);self.assertEqual(line,'字'*50)

    def test_old_key_taps_are_ignored_and_bad_reports_rejected(self):
        self.bridge.handle({'report':1,'hex':'4000000000000000'})
        self.bridge.handle(self.packet(1,0,generation=6))
        self.assertEqual(self.bridge.stats['stale_reports'],1)
        self.bridge.handle(self.packet(1,0,struct.pack('<IBB',16000,16,1)))
        with self.assertRaises(ValueError): self.bridge.handle(self.packet(5,3))
        self.assertEqual(self.bridge.stats['sequence_errors'],1)

    def test_paste_restores_the_clipboard_unless_it_changed(self):
        board=Mock();board.changeCount.return_value=5
        item=Mock();item.types.return_value=['public.utf8-plain-text'];item.dataForType_.return_value=b'old'
        board.pasteboardItems.return_value=[item]
        appkit=SimpleNamespace(NSPasteboard=SimpleNamespace(generalPasteboard=lambda:board),NSPasteboardTypeString='public.utf8-plain-text',
            NSPasteboardItem=SimpleNamespace(alloc=lambda:SimpleNamespace(init=lambda:Mock())))
        keys=Mock()
        with patch.dict(sys.modules,{'AppKit':appkit}),patch('passport_dictation.time.sleep'):
            passport_dictation.paste('你好',keys)()
        keys.assert_called_once_with('paste')
        board.setString_forType_.assert_any_call('你好','public.utf8-plain-text')
        board.writeObjects_.assert_called_once()
        board.changeCount.side_effect=[5,6];board.writeObjects_.reset_mock()
        with patch.dict(sys.modules,{'AppKit':appkit}),patch('passport_dictation.time.sleep'):
            passport_dictation.paste('你好',keys)()
        board.writeObjects_.assert_not_called()

    def test_opus_encoder_makes_frames(self):
        frame=passport_dictation.Encoder().encode(bytes(FRAME))
        self.assertTrue(0<len(frame)<200)


class TranscriberIdentityTests(TestCase):
    def test_second_device_derived_from_the_board_mac(self):
        import passport_device_cloud
        board={'mac':'10:20:30:40:50:60','xiaozhi':{'url':'wss://x/v1/','token':'t','client_id':'chat-client'}}
        identity=passport_device_cloud.transcriber(board)
        self.assertEqual(identity,{'mac':'12:20:30:40:50:60','xiaozhi':{'url':'wss://x/v1/','token':'t','client_id':'61692d70-6173-4000-8000-122030405060'}})
        reply=Mock();reply.read.return_value=json.dumps({'websocket':{'url':'wss://ota/v1/','token':'shared'}}).encode()
        opener=Mock();opener.open.return_value=reply
        with patch('urllib.request.build_opener',return_value=opener):
            fresh=passport_device_cloud.transcriber({'mac':'10:20:30:40:50:60'})
        self.assertEqual(fresh['xiaozhi'],{'url':'wss://ota/v1/','token':'shared','client_id':'61692d70-6173-4000-8000-122030405060'})
        sent=opener.open.call_args.args[0]
        self.assertEqual((sent.get_header('Device-id'),sent.get_header('Client-id')),('12:20:30:40:50:60','61692d70-6173-4000-8000-122030405060'))
