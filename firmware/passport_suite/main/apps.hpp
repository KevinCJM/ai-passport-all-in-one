#pragma once
#include <array>
#include <cstdint>
namespace passport {
enum class App : uint8_t { Home, Chat, Dictation, Codex, Focus, Monitor, Menu=7, RetiredOtp, Radio, Settings, Words, Slides, Alert, Status, Calendar, Countdown, Torch, Qr, News, Sonar, Rate, Stopwatch, History, Daily, Quake, Quiz, Hacker, Wifi, Sound, Motion, Podcast, Forecast, Noise, Alarm, Stock, Typhoon, Hot, Memo, Wheel, Translate, Guard, Todo, Ledger, Calc, Voice, Walkie, Macro, Count };
enum class SettingsPage : uint8_t { List, Wifi, Bluetooth, Brightness, Volume, Idle, Wake, City };
inline constexpr uint8_t kSettingsRows = 7;
// Menu order: themed pages of up to nine (3 x 3), each row of three alike; kMenuPages names them and kMenuStarts
// holds each page's first entry. The voice pages come first, 系统设置 ends the last page.
inline constexpr std::array<App,43> kFunctions{
    App::Chat,App::Memo,App::Dictation, App::Translate,App::Ledger,App::Calc, App::Todo,App::Alarm,App::Focus,
    App::Codex,App::Monitor,App::Guard, App::Slides,App::Status,App::Qr, App::Stopwatch,App::Countdown,App::Calendar,
    App::Forecast,App::Typhoon,App::Quake, App::Stock,App::Macro,App::Rate, App::News,App::Hot,App::Hacker,
    App::Words,App::Daily,App::Quiz, App::Radio,App::Podcast,App::Noise, App::Sound,App::History,
    App::Torch,App::Alert,App::Wheel, App::Motion,App::Wifi,App::Sonar, App::Walkie,App::Settings};
inline constexpr const char* kMenuPages[]{"小智 · 语音","电脑 · 时间","天气 · 财经 · 资讯","学习 · 影音","工具 · 设置"};
inline constexpr uint8_t kMenuStarts[]{0,9,18,27,35,kFunctions.size()};  // the last marks the end
static_assert(std::size(kMenuStarts)==std::size(kMenuPages)+1);
inline constexpr unsigned menuPageOf(unsigned index){unsigned p=0;while(index>=kMenuStarts[p+1])++p;return p;}
// 百度热搜: the items kept, and the tag of the pinned one (Baidu's own tags: 1 新, 3 热).
inline constexpr unsigned kHotRows = 20;
inline constexpr uint8_t kHotPinned = 255;
// Warning light frames: one colour per 125 ms step, 0 = dark. Up/Down picks the pattern.
struct AlertMode { const char* name; uint8_t count; uint32_t colors[14]; };
inline constexpr AlertMode kAlertModes[]{
    {"红蓝爆闪",8,{0xff0000,0,0xff0000,0,0x0030ff,0,0x0030ff,0}},
    {"七彩闪烁",14,{0xff0000,0,0xff8000,0,0xffff00,0,0x00ff00,0,0x00ffff,0,0x0030ff,0,0xff00ff,0}},
    {"红色频闪",2,{0xff0000,0}},
    {"黄色频闪",2,{0xffb000,0}},
    {"白色频闪",2,{0xffffff,0}},
};
// 白噪音 (NoiseGen kinds in order) and the sleep timer's choices in minutes (收音机 and 播客 share them).
struct NoiseSound { const char* name; const char* note; };
inline constexpr NoiseSound kNoiseSounds[]{{"雨声","细雨沙沙，夹着雨滴"},{"海浪","潮起潮落，约 8 秒一浪"},{"粉红噪音","柔和均匀，适合专注"},{"棕噪音","低沉厚重，适合入睡"},{"白噪音","明亮的沙沙声，遮住人声"}};
inline constexpr uint8_t kNoiseTimers[]{0,15,30,60,90};
constexpr uint8_t nextSleepTimer(uint8_t minutes){unsigned at=0;while(at<std::size(kNoiseTimers)&&kNoiseTimers[at]!=minutes)++at;return kNoiseTimers[(at+1)%std::size(kNoiseTimers)];}
static_assert(nextSleepTimer(0)==15&&nextSleepTimer(60)==90&&nextSleepTimer(90)==0&&nextSleepTimer(7)==15);
// Status sign presets: text ('\n' breaks the line), English tag, card colour.
// generate_passport_font.py draws the 48 px status font from the characters between the markers.
struct StatusLabel { const char* text; const char* tag; uint32_t color; };
// status-font-begin
inline constexpr StatusLabel kStatusLabels[]{
    {"开会中","IN A MEETING",0xe5484d},
    {"专注中\n勿扰","DO NOT DISTURB",0xe5484d},
    {"电话中","ON A CALL",0xe5484d},
    {"蹲坑中","BE RIGHT BACK",0xf59e0b},
    {"吃饭中","LUNCH BREAK",0xf59e0b},
    {"马上回来","BE RIGHT BACK",0xf59e0b},
    {"外出中","OUT OF OFFICE",0xf59e0b},
    {"午休中\n勿扰","NAP TIME",0x8b5cf6},
    {"思考人生","THINKING",0x8b5cf6},
    {"欢迎打扰","AVAILABLE",0x22c55e},
    {"摸鱼中","SLACKING",0x22c55e},
    {"改Bug中","DEBUGGING",0x3b82f6},
    {"编译中\n别催","BUILDING",0x3b82f6},
    {"加班中","WORKING LATE",0x3b82f6},
    {"面试中","INTERVIEW",0xe5484d},
    {"已下班","OFF DUTY",0x64748b},
};
// status-font-end
// Local pages where Confirm-to-Confirm speech becomes one line of text (语音笔记's cloud task, one phrase a turn):
// 状态牌's label, 随机转盘's mode or options, a todo, a ledger entry, an alarm, a 倒数日 event, a sum to work out.
constexpr bool speaksOnce(App app){return app==App::Status||app==App::Wheel||app==App::Todo||app==App::Ledger||app==App::Calc||app==App::Alarm||app==App::Countdown;}
// Pages that also take one spoken phrase ("speak" to the device's task) though they are not local: 汇率 and 自选股
// (a long Up), and Settings > 所在城市.
constexpr bool speaksToo(App app){return app==App::Rate||app==App::Stock||app==App::Settings;}
// Pages whose cloud connection stays open between phrases (Confirm starts and ends each): 语音笔记 and 中英翻译.
constexpr bool keepsListening(App app){return app==App::Memo||app==App::Translate;}
// 语音指令 (App::Voice, not in the menu): 你好小智 opens it; one phrase, heard through 语音笔记's own XiaoZhi identity so
// nothing reaches 小智语音's history or memory, opens a badge page or a Mac program.
constexpr bool needsComputer(App app){return app==App::Dictation||app==App::Codex||app==App::Monitor||app==App::Guard;}
constexpr bool needsInternet(App app){return app==App::Home||app==App::Chat||app==App::Radio||app==App::Words||app==App::News||app==App::Rate||app==App::History||app==App::Daily||app==App::Quake||app==App::Quiz||app==App::Hacker||app==App::Podcast||app==App::Forecast||app==App::Stock||app==App::Macro||app==App::Typhoon||app==App::Hot||app==App::Memo||app==App::Translate||app==App::Voice;}
// 汇率: currencies offered on the phone setup page and by voice (ISO code, Chinese name).
struct Currency { const char* code; const char* name; };
inline constexpr Currency kCurrencies[]{{"CNY","人民币"},{"USD","美元"},{"EUR","欧元"},{"JPY","日元"},{"HKD","港币"},{"GBP","英镑"},{"KRW","韩元"},
    {"TWD","新台币"},{"MOP","澳门元"},{"SGD","新加坡元"},{"AUD","澳元"},{"CAD","加元"},{"NZD","新西兰元"},{"CHF","瑞士法郎"},{"THB","泰铢"},
    {"MYR","林吉特"},{"VND","越南盾"},{"PHP","菲律宾比索"},{"IDR","印尼盾"},{"INR","印度卢比"},{"RUB","卢布"},{"AED","迪拉姆"},{"SEK","瑞典克朗"},
    {"TRY","土耳其里拉"},{"BRL","巴西雷亚尔"},{"MXN","墨西哥比索"},{"ARS","阿根廷比索"},{"CLP","智利比索"},{"ZAR","南非兰特"},{"EGP","埃及镑"},
    {"SAR","沙特里亚尔"},{"QAR","卡塔尔里亚尔"},{"KWD","科威特第纳尔"},{"ILS","以色列谢克尔"},{"NOK","挪威克朗"},{"DKK","丹麦克朗"},{"PLN","波兰兹罗提"},
    {"CZK","捷克克朗"},{"HUF","匈牙利福林"},{"UAH","乌克兰格里夫纳"},{"KZT","哈萨克坚戈"},{"MNT","蒙古图格里克"},{"LAK","老挝基普"},{"KHR","柬埔寨瑞尔"},
    {"MMK","缅甸元"},{"NPR","尼泊尔卢比"},{"LKR","斯里兰卡卢比"},{"PKR","巴基斯坦卢比"},{"BDT","孟加拉塔卡"}};
constexpr const char* currencyName(const char* code){for(auto& c:kCurrencies){const char *a=c.code,*b=code;while(*a&&*a==*b){++a;++b;}if(!*a&&!*b)return c.name;}return code;}
// Torch colours (白光, 暖光, 红光 keeps night vision); Up/Down steps the brightness by 20%.
inline constexpr uint32_t kTorchColors[]{0xffffff,0xffc070,0xff2000};
inline constexpr const char* kTorchNames[]{"白光","暖光","红光"};
constexpr const char* computerConnectionHint(bool usb,bool bluetooth,bool assistant){
    if(!usb&&!bluetooth)return "请连接电脑蓝牙";
    return assistant?nullptr:"请启动电脑助手";
}
}
