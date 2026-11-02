"""记分牌 rules (score.hpp) on the host: replay, serve, deuce, caps, deciding games, undo and the 自由计分 fold."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "score.hpp"
#include <cassert>
#include <cstdio>
using namespace passport;
static void play(ScoreBoard& s,const char* sides){for(;*sides;++sides)assert(scoreAdd(s,*sides=='b'));}
static void repeat(ScoreBoard& s,const char* sides,int n){while(n--)play(s,sides);}
int main(){
    ScoreBoard s;s.rule=1;  // 乒乓球
    auto t=scoreReplay(s);assert(t.server==0&&t.chance==ScoreState::kNone);
    play(s,"aa");assert(scoreReplay(s).server==1);play(s,"aa");assert(scoreReplay(s).server==0);
    repeat(s,"a",7);t=scoreReplay(s);assert(t.games[0]==1&&t.history[0][0]==11&&t.history[0][1]==0&&t.server==1);  // game 2: 蓝方 serves first
    repeat(s,"ab",10);t=scoreReplay(s);assert(t.points[0]==10&&t.points[1]==10&&t.server==1);
    play(s,"a");assert(scoreReplay(s).server==0);play(s,"b");assert(scoreReplay(s).server==1);play(s,"bb");
    t=scoreReplay(s);assert(t.games[1]==1&&t.history[1][0]==11&&t.history[1][1]==13);
    repeat(s,"a",22);t=scoreReplay(s);assert(t.winner==0&&t.games[0]==3&&t.done==4);
    assert(!scoreAdd(s,0)&&scoreUndo(s)&&scoreReplay(s).winner==ScoreState::kNone);
    s={};s.rule=3;  // 排球: four sets to 25, the fifth to 15
    repeat(s,"a",25);repeat(s,"b",25);repeat(s,"a",25);repeat(s,"b",25);t=scoreReplay(s);assert(t.games[0]==2&&t.games[1]==2&&t.server==0);
    repeat(s,"ab",14);t=scoreReplay(s);assert(t.points[0]==14&&t.chance==ScoreState::kNone);play(s,"a");assert(scoreReplay(s).chance==0&&scoreReplay(s).matchPoint);
    play(s,"a");t=scoreReplay(s);assert(t.winner==0&&t.history[4][0]==16&&t.history[4][1]==14);
    s={};s.rule=2;repeat(s,"ab",29);play(s,"b");t=scoreReplay(s);assert(t.games[1]==1&&t.history[0][1]==30);  // 羽毛球 cap
    s={};repeat(s,"ab",700);t=scoreReplay(s);assert(t.points[0]==700&&t.points[1]==700&&s.count<=ScoreBoard::kPoints&&s.valid());
    while(scoreUndo(s)){}t=scoreReplay(s);assert(t.points[0]+t.points[1]==s.base[0]+s.base[1]&&!s.fresh());
    s={};repeat(s,"a",999);assert(!scoreAdd(s,0)&&scoreAdd(s,1));
    ScoreBoard bad;bad.rule=9;assert(!bad.valid());bad={};bad.rule=1;bad.base[0]=1;assert(!bad.valid());
    puts("SCORE_OK");
}
'''
class ScoreRules(unittest.TestCase):
    def test_rules(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder); (folder/'t.cpp').write_text(SOURCE)
            subprocess.run([shutil.which('clang++') or 'c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I'+str(ROOT/'firmware/passport_suite/main'), str(folder/'t.cpp'), '-o', str(folder/'t')], check=True)
            self.assertIn('SCORE_OK', subprocess.run([str(folder/'t')], check=True, capture_output=True, text=True).stdout)
if __name__ == '__main__': unittest.main()
