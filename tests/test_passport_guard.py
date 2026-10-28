"""离座锁屏 decisions without Bluetooth, a real lock or caffeinate: a fake clock and signal, recorded side effects."""
import sys
from pathlib import Path
from unittest import TestCase

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from passport_guard import Guard,PRESENT,LEAVING,LOCKED,USB,ABSOLUTE


class FakeAwake:
    def __init__(self):self.held=[]
    def hold(self,wanted):self.held.append(wanted)


class World:
    def __init__(self):
        self.t=1000.0;self.signal=-50;self.locks=[];self.wakes=0;self.nudges=0;self.idle=0;self.saved=0;self.screen=False
        self.settings={};self.awake=FakeAwake()
        self.guard=Guard(lambda:self.signal,self.settings,self.save,lock=self.lock,wake=self.wake,
                         nudge=self.nudge,idle=lambda:self.idle,awake=self.awake,now=lambda:self.t,locked=lambda:self.screen)
    def lock(self):self.locks.append(self.t);self.screen=True
    def save(self):self.saved+=1
    def wake(self):self.wakes+=1
    def nudge(self):self.nudges+=1
    def run(self,seconds,signal=None):
        if signal is not None:self.signal=signal
        for _ in range(seconds):self.t+=1;state=self.guard.tick()
        return state


class GuardTests(TestCase):
    def test_locks_after_the_delay_below_the_line_and_wakes_on_return(self):
        w=World();w.guard.configure('cfg:1101')  # 中, 10 s, keep off, wake on return
        self.assertEqual(w.run(3)['guard']['phase'],PRESENT)
        # The median of five samples: two weak readings do not count yet.
        state=w.run(2,signal=-90);self.assertEqual(state['guard']['phase'],PRESENT)
        state=w.run(3);self.assertEqual(state['guard']['phase'],LEAVING);self.assertTrue(0<state['guard']['left']<=10)
        self.assertEqual(w.locks,[])
        w.run(10);self.assertEqual(len(w.locks),1);self.assertEqual(w.guard.phase,LOCKED)
        w.run(5);self.assertEqual(len(w.locks),1,'locks once')
        # Just over the line is not back; over it by the margin for three seconds lights the screen, once.
        line=ABSOLUTE[1];w.run(8,signal=line+1);self.assertEqual(w.wakes,0)
        w.run(8,signal=line+10);self.assertEqual(w.wakes,1);self.assertEqual(w.guard.phase,LOCKED,'still locked until unlocked')
        w.run(5);self.assertEqual(w.wakes,1)
        w.screen=False;w.run(1);self.assertEqual(w.guard.phase,PRESENT)

    def test_unlocking_without_the_badge_locks_again(self):
        w=World();w.guard.configure('cfg:1001');w.run(3)
        w.run(10,signal=-95);self.assertEqual(len(w.locks),1)
        w.screen=False;w.run(1);self.assertEqual(w.guard.phase,LEAVING);w.run(6);self.assertEqual(len(w.locks),2)

    def test_a_lost_link_locks_too(self):
        w=World();w.guard.configure('cfg:1001');w.run(3)
        w.signal=None;w.run(4);self.assertEqual(w.locks,[])
        w.run(2);self.assertEqual(len(w.locks),1)

    def test_calibration_moves_the_line(self):
        w=World();w.run(5,signal=-58);w.guard.configure('cal:2101')
        self.assertEqual(w.settings['guard_baseline'],-58);self.assertEqual(w.saved,1)
        self.assertEqual(w.guard.line(),-58-18)
        w.guard.configure('cfg:0101');self.assertEqual(w.guard.line(),-58-8)
        w.run(20,signal=-70);self.assertEqual(len(w.locks),1,'under 近 after calibration')

    def test_keep_awake_only_while_here_and_nudges_when_idle(self):
        w=World();w.guard.configure('cfg:1121')
        w.run(3);self.assertTrue(w.awake.held[-1])
        w.idle=200;w.run(1);self.assertEqual(w.nudges,1);w.run(30);self.assertEqual(w.nudges,1,'at most once a minute')
        w.run(31);self.assertEqual(w.nudges,2)
        w.idle=10;w.run(61);self.assertEqual(w.nudges,2,'not while you are typing')
        w.run(30,signal=-95);self.assertFalse(w.awake.held[-1],'no keep-awake once away')
        w.guard.configure('cfg:1101');w.run(20,signal=-50);self.assertFalse(w.awake.held[-1])

    def test_usb_keeps_awake_and_never_counts_as_away(self):
        w=World();w.guard.configure('cfg:1011');w.signal='usb'
        state=w.run(60);self.assertEqual(state['guard']['phase'],USB);self.assertEqual(w.locks,[]);self.assertTrue(w.awake.held[-1])
        w.screen=True;w.run(1);self.assertFalse(w.awake.held[-1],'not over a locked screen');w.screen=False;w.run(1)
        w.guard.configure('lock:1011');self.assertEqual(len(w.locks),1);self.assertEqual(w.guard.phase,LOCKED)
        w.run(10);self.assertEqual(w.guard.phase,LOCKED);w.screen=False;w.run(1);self.assertEqual(w.guard.phase,USB)

    def test_dry_run_records_instead_of_locking(self):
        w=World();w.settings['guard_dry_run']=True;w.guard.configure('lock:1101')
        self.assertEqual(w.locks,[]);self.assertIn('would lock: manual',w.guard.events);self.assertEqual(w.guard.phase,LOCKED)
        w.signal='usb';w.run(3);self.assertEqual(w.guard.phase,LOCKED);w.run(3);self.assertEqual(w.guard.phase,USB)

    def test_bad_settings_are_ignored(self):
        w=World();w.guard.configure('cfg:9999');self.assertEqual((w.guard.distance,w.guard.delay,w.guard.keep),(1,1,2))
        w.guard.configure('cfg:12');w.guard.configure('nonsense');self.assertEqual(w.guard.delay,1)
