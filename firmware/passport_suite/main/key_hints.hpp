#pragma once
#include "apps.hpp"
namespace passport {
// alt: the calendar shows a day's 黄历, the stopwatch runs, or the quiz answer is revealed.
inline const char* keyHint(App app,SettingsPage page,bool focusRunning,bool pairing,bool loading,bool error,bool alt=false,unsigned tab=0){
    if(pairing)return "确认：允许配对  |  长按确认：取消";
    if(app==App::Home)return "上下：切换  |  确认：功能列表  |  长按确认：息屏";
    if(app==App::Menu)return "上下：选择 | 长按上下：翻页 | 确认：进入";
    if(app==App::Settings){
        if(page==SettingsPage::List)return "上下：选择  |  确认：进入  |  长按确认：返回";
        // tab: 0 the setup hotspot is on, 1 添加 WiFi, 2 a saved network, 3 its delete asked.
        if(page==SettingsPage::Wifi){static const char* const hints[]{"确认：关闭配网热点  |  长按确认：返回","上下：选择  |  确认：手机配网  |  长按确认：返回","上下：选择  |  确认：连接  |  长按下：删除","长按下：确认删除  |  其他键：取消"};return hints[tab%4];}
        if(page==SettingsPage::Bluetooth)return "确认：开关蓝牙  |  长按确认：返回";
        // tab: 0 the time picked, 1 连电脑时常亮 picked, 2 changing the time.
        if(page==SettingsPage::Idle){static const char* const hints[]{"上下：选择  |  确认：修改  |  长按确认：返回","上下：选择  |  确认：开关  |  长按确认：返回","上下：调整  |  确认：保存  |  长按确认：取消"};return hints[tab%3];}
        if(page==SettingsPage::Wake)return "上下：选择  |  确认：开关  |  长按确认：返回";
        // tab: 0 ready, 1 listening, 2 recognising.
        if(page==SettingsPage::City){static const char* const hints[]{"上下：选择  |  确认：开始  |  长按确认：返回","确认：结束并设置","正在识别，请稍候"};return hints[tab%3];}
        return "上下：调整  |  确认：保存  |  长按确认：取消并返回";
    }
    // One line: a scrolling footer redraws 8 times a second.
    if(app==App::Focus)return focusRunning?"确认：暂停":"上下：切换  |  确认：开始";
    // Chat fits one line: a scrolling footer redraws 8 times a second while audio loads the CPU.
    if(app==App::Chat)return error?"上下：音量  |  确认：重试":loading?"上下：音量  |  长按确认：返回":"上下：音量  |  确认：说话";
    // The word card is local; only reading aloud waits for the network.
    if(app==App::Words)return "上下：切换  |  确认：朗读  |  长按上：认识  |  长按下：不认识  |  长按确认：返回";
    // Local keys; the BLE link state is on the page itself.
    if(app==App::Slides)return "上下：翻页  |  确认：开始放映  |  长按上：黑屏  |  长按下：结束放映  |  长按确认：返回";
    if(app==App::Calendar)return tab==2?"上下：前后一天  |  确认：返回月历":alt?"上下：前后一天  |  确认：月相节气":"上下：日 | 长按：月 | 确认：黄历";
    if(app==App::Stopwatch)return alt?"确认：暂停  |  上：计次":"确认：开始  |  下：复位";
    // 汇率, 自选股 (its list) and 倒数日 tab: 0 the page, 1 listening, 2 recognising, 3 a delete asked.
    static const char* const spoken[]{"","确认：结束并添加  |  长按确认：放弃","正在识别，请稍候","长按下：确认删除  |  其他键：取消"};
    if(app==App::Countdown)return tab%4?spoken[tab%4]:"上下：切换 | 确认：列表 | 长按上：说话添加 | 长按下：删除";
    if(app==App::Torch)return "上下：亮度  |  确认：颜色";
    if(app==App::Qr)return "上下：切换  |  确认：提亮屏幕";
    if(app==App::Sonar)return "上下：选设备  |  确认：雷达 / 列表  |  长按上下：有 / 未标识  |  长按确认：返回";
    if(app==App::Rate&&(tab%4||!error))return tab%4?spoken[tab%4]:"上下：选择 | 确认：刷新 | 长按上：说话添加 | 长按下：删除";
    if(app==App::News&&!error)return "上下：翻看 / 切换  |  确认：刷新";
    if(app==App::History&&!error)return "上下：切换  |  确认：刷新";
    if(app==App::Daily&&!error)return "上：前一天  |  下：后一天  |  确认：朗读";
    if(app==App::Quake&&!error)return "上下：切换  |  确认：刷新";
    if(app==App::Hacker&&!error)return "上下：切换  |  确认：刷新";
    // alt: the 预警 tab with warnings, where Confirm opens each one's text.
    if(app==App::Forecast&&!error)return alt?"上下：切换  |  确认：原文":"上下：切换  |  确认：刷新";
    if(app==App::Typhoon&&!error)return "上下：切换台风  |  确认：刷新";
    // alt: an item's detail is open.
    if(app==App::Hot&&!error)return alt?"上下：切换  |  确认：返回列表":"上下：选择  |  确认：详情";
    // tab: 0 the 录音 row, 1 a note in the list, 2 an open note, 3 its delete asked, 4 listening, 5 recognising.
    // 待办提醒 tab: 0 the 录音 row, 1 a todo, 2 its delete asked, 3 listening, 4 recognising, 5 a reminder going off.
    if(app==App::Todo){static const char* const hints[]{"上下：选择  |  确认：说一条待办  |  长按上：清除已完成","上下：选择  |  确认：完成 / 取消  |  长按下：删除  |  长按上：清除已完成",
        "长按下：确认删除  |  其他键：取消","确认：结束并添加  |  长按确认：放弃","正在识别，请稍候","确认：完成  |  上下：10 分钟后再提醒  |  长按确认：关闭"};return hints[tab%6];}
    // 语音记账 tab: 0 the 录音 row, 1 an entry, 2 its delete asked, 3 listening, 4 recognising.
    if(app==App::Ledger){static const char* const hints[]{"上下：选择  |  确认：记一笔  |  长按确认：返回","上下：选择  |  确认：改分类  |  长按下：删除",
        "长按下：确认删除  |  其他键：取消","确认：结束并记账  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%5];}
    // 语音计算器 tab: 0 ready, 1 listening, 2 recognising.
    if(app==App::Calc){static const char* const hints[]{"确认：说算式  |  上下：翻看结果  |  长按确认：返回","确认：结束并计算  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%3];}
    // 语音指令: tab 0 waiting, 1 listening, 2 recognising.
    if(app==App::Voice){static const char* const hints[]{"确认：再说一次  |  长按确认：返回","确认：说完了  |  长按确认：返回","正在识别，请稍候"};return error?"确认：重试  |  长按确认：返回":loading?"正在连接  |  长按确认：返回":hints[tab%3];}
    // 对讲机 tab: 0 idle, 1 talking, 2 hearing someone; alt: a call opened the page (long Confirm goes back to the last one).
    // Short enough not to scroll: a scrolling footer redraws 8 times a second while Opus runs.
    if(app==App::Walkie){static const char* const hints[]{"上下：音量 | 确认：说话","确认：说完了 | 长按确认：返回","对方说话中 | 长按确认：返回"};return alt&&tab!=1?"任意键：留下 | 长按确认：回去":hints[tab%3];}
    if(app==App::Guard)return "上下：选项  |  确认：修改  |  长按上：校准  |  长按下：立即锁屏  |  长按确认：返回";
    // 中英翻译 tab: 0 ready, 1 listening, 2 recognising.
    if(app==App::Translate&&!error){static const char* const hints[]{"确认：说话  |  上下：滚动  |  长按上：朗读  |  长按确认：返回","确认：结束并翻译  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%3];}
    if(app==App::Memo&&!error){static const char* const hints[]{"上下：选择  |  确认：开始录音","上下：选择  |  确认：查看  |  长按下：删除","上下：翻页  |  确认：返回  |  长按下：删除","长按下：确认删除  |  其他键：取消","确认：结束并保存  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%6];}
    // alt: a stock's detail is open.
    if(app==App::Stock&&(tab%4||!error))return alt?"上下：切换  |  确认：分时/日K  |  长按确认：列表":tab%4?spoken[tab%4]:"上下：选择 | 确认：详情 | 长按上：说话添加 | 长按下：删除";
    if(app==App::Macro)return alt?"上下：切换 | 确认：12/60期 | 长按上：刷新 | 长按确认：列表":"上下：选择 | 确认：详情 | 长按上下：换页 | 长按确认：返回";
    // alt: the sleep timer row is selected (白噪音), the alarm is ringing (闹钟).
    if(app==App::Noise)return alt?"上下：选择  |  确认：切换定时  |  长按上下：音量":"上下：选择  |  确认：播放/停止  |  长按上下：音量";
    // 闹钟 tab: 0 the 录音 row, 1 an alarm, 2 its delete asked, 3 listening, 4 recognising, 5 an alarm's clock open.
    if(app==App::Alarm){static const char* const hints[]{"上下：选择  |  确认：说闹钟时间  |  长按确认：返回","上下：选择  |  确认：调整  |  长按下：删除","长按下：确认删除  |  其他键：取消",
        "确认：结束并设置  |  长按确认：放弃","正在识别，请稍候","上下：±5 分钟 | 长按上下：±1 小时 | 确认：重复 | 长按确认：列表"};return alt?"上下：贪睡 5 分钟  |  确认：关闭":hints[tab%6];}
    if(app==App::Quiz&&!error)return alt?"确认：下一题":"上下：选答案  |  确认：作答";
    // alt picks the tab / view: WiFi 0 信道 1 延迟 2 设备 3 唤醒, sound 0 频谱 1 调音 2 节拍.
    if(app==App::Motion)return "上下：灵敏度 | 确认：校准 | 长按上下：提示音";
    if(app==App::Wifi){static const char* const hints[]{"上下：选网络 | 确认：重扫 | 长按上下：换页","确认：重测 | 长按上下：换页","上下：选设备 | 确认：设为唤醒目标 | 长按上下：换页","确认：发送唤醒 | 长按上下：换页"};return hints[tab%4];}
    if(app==App::Sound){static const char* const hints[]{"上下：校准 | 确认：清最大值 | 长按上下：换页","上下：标准音 A4 | 长按上下：换页","上下：速度 | 确认：开始/停止 | 长按上下：换页"};return hints[tab%3];}
    // tab: 0 the sign, 1 listening, 2 recognising (as 语音输入).
    if(app==App::Status){static const char* const hints[]{"上下：切换  |  确认：说话  |  长按上下：常亮","确认：结束并显示  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%3];}
    // tab: 0 the wheel, 1 listening, 2 recognising, 3 a mode's delete asked.
    if(app==App::Wheel){static const char* const hints[]{"上下：换模式  |  确认：转  |  长按上：说话添加  |  长按下：删除模式","确认：结束并添加  |  长按确认：放弃","正在识别，请稍候","再长按下：删除这个模式  |  其他键：取消"};return hints[tab%4];}
    if(app==App::Alert)return "上下：切换  |  长按确认：退出";
    if(loading)return "长按确认：返回";
    if(error)return "确认：重试  |  长按确认：返回";
    // tab: 0 ready, 1 listening, 2 recognising.
    if(app==App::Dictation){static const char* const hints[]{"确认：说话  |  下：回车  |  上：删除整行  |  长按确认：返回","确认：结束并输入  |  长按确认：放弃","正在识别，请稍候"};return hints[tab%3];}
    if(app==App::Radio)return "上下：音量  |  长按上：换台  |  长按下：定时关闭  |  确认：播放/停止  |  长按确认：返回";
    // alt: a program is open in the player.
    if(app==App::Podcast)return alt?"上下：音量  |  长按上：换一期  |  长按下：定时关闭  |  确认：播放/暂停  |  长按确认：节目列表":"上下：选择  |  确认：收听  |  长按确认：返回";
    return "确认：刷新  |  长按确认：返回";
}
}
