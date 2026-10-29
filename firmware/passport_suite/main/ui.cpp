#include "suite.hpp"
#include "ui_pool_buffer.hpp"
#include "home_ui.hpp"
#include "status_bar.hpp"
#include "key_hints.hpp"
#include "word_list.hpp"
#include "lunar.hpp"
#include "moon.hpp"
#include "todo_time.hpp"
#include "typhoon_map.hpp"
#ifdef PASSPORT_DEVICE
#include "board.hpp"
#endif
#include "esp_timer.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iterator>
#include <tuple>
#include <type_traits>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
bool passport_ui_font_covers_all();
extern const lv_font_t passport_ui_font, passport_status_font, passport_ui_font_x2, passport_ui_font_x3, passport_icon_font_20, passport_icon_font_24;
// Icons LVGL's symbol set lacks (status_font.cpp); badges draw with these fonts, which fall back to Montserrat.
#define ICON_ROBOT "\xEE\x80\x81"
#define ICON_TORCH "\xEE\x80\x82"
#define ICON_WEATHER "\xEE\x80\x83"
#define ICON_CLOCK "\xEE\x80\x84"
#define ICON_WIND "\xEE\x80\x85"
#define ICON_CHART "\xEE\x80\x86"
#define ICON_TYPHOON "\xEE\x80\x87"
#define ICON_FIRE "\xEE\x80\x88"
#define ICON_MIC "\xEE\x80\x89"
#define ICON_WALLET "\xEE\x80\x8A"
#define ICON_CALC "\xEE\x80\x8B"
namespace {
// Status sign fallback: the 16 px UI font drawn three times larger, so any GB2312 text typed on the
// computer still shows at sign size. Softer than the 48 px preset glyphs, but needs no extra flash.
// Twice as large it heads the 月相 view and the 转盘's pick.
template<int kScale> bool scaledGlyph(const lv_font_t*,lv_font_glyph_dsc_t* g,uint32_t letter,uint32_t next){
    if(!lv_font_get_glyph_dsc_fmt_txt(&passport_ui_font,g,letter,next))return false;
    g->adv_w*=kScale;g->box_w*=kScale;g->box_h*=kScale;g->ofs_x*=kScale;g->ofs_y*=kScale;
    g->format=LV_FONT_GLYPH_FORMAT_A8;
    return true;
}
template<int kScale> const void* scaledBitmap(lv_font_glyph_dsc_t* g,lv_draw_buf_t* out){
    static uint8_t small[32*32];lv_draw_buf_t source;
    lv_font_glyph_dsc_t glyph=*g;glyph.resolved_font=&passport_ui_font;glyph.stride=0;
    const int w=g->box_w/kScale,h=g->box_h/kScale,stride=lv_draw_buf_width_to_stride(w,LV_COLOR_FORMAT_A8);
    if(w*h>static_cast<int>(sizeof(small))||lv_draw_buf_init(&source,w,h,LV_COLOR_FORMAT_A8,stride,small,sizeof(small))!=LV_RESULT_OK
       ||!lv_font_get_bitmap_fmt_txt(&glyph,&source))return nullptr;
    // Bilinear: each output pixel samples the source at its centre, in 1/256 source pixels.
    const auto at=[&](int x,int y){return small[std::clamp(y,0,h-1)*stride+std::clamp(x,0,w-1)];};
    for(int y=0;y<g->box_h;++y)for(int x=0;x<g->box_w;++x){
        const int sx=(x*2+1)*128/kScale-128,sy=(y*2+1)*128/kScale-128;
        const int x0=sx>>8,y0=sy>>8,fx=sx&255,fy=sy&255;
        const int top=at(x0,y0)*(256-fx)+at(x0+1,y0)*fx,bottom=at(x0,y0+1)*(256-fx)+at(x0+1,y0+1)*fx;
        out->data[y*out->header.stride+x]=(top*(256-fy)+bottom*fy)>>16;
    }
    return out;
}
}
template<int kScale> constexpr lv_font_t scaledFont(){
    lv_font_t f{};
    f.get_glyph_dsc=scaledGlyph<kScale>;f.get_glyph_bitmap=scaledBitmap<kScale>;
    f.line_height=20*kScale;f.base_line=4*kScale;f.underline_position=-2*kScale;f.underline_thickness=kScale;
    return f;
}
const lv_font_t passport_ui_font_x2=scaledFont<2>(),passport_ui_font_x3=scaledFont<3>();

namespace passport {
namespace {
// LVGL restyles and invalidates an object on every style write, even one that changes nothing.
// Render functions run on every state change, so they use these setters that skip unchanged
// writes; otherwise each render repaints most of the screen.
template<class T> bool same(lv_obj_t* obj,lv_style_prop_t prop,lv_style_selector_t selector,T wanted){
    lv_style_value_t value;
    if(lv_obj_get_local_style_prop(obj,prop,&value,selector)!=LV_STYLE_RES_FOUND)return false;
    if constexpr(std::is_same_v<T,lv_color_t>)return lv_color_eq(value.color,wanted);
    else if constexpr(std::is_pointer_v<T>)return value.ptr==wanted;
    else return value.num==static_cast<int32_t>(wanted);
}
void setTextColor(lv_obj_t* o,lv_color_t v,lv_style_selector_t s){if(!same(o,LV_STYLE_TEXT_COLOR,s,v))lv_obj_set_style_text_color(o,v,s);}
void setBgColor(lv_obj_t* o,lv_color_t v,lv_style_selector_t s){if(!same(o,LV_STYLE_BG_COLOR,s,v))lv_obj_set_style_bg_color(o,v,s);}
void setTextFont(lv_obj_t* o,const lv_font_t* v,lv_style_selector_t s){if(!same(o,LV_STYLE_TEXT_FONT,s,v))lv_obj_set_style_text_font(o,v,s);}
void setTextAlign(lv_obj_t* o,lv_text_align_t v,lv_style_selector_t s){if(!same(o,LV_STYLE_TEXT_ALIGN,s,v))lv_obj_set_style_text_align(o,v,s);}
void setArcColor(lv_obj_t* o,lv_color_t v,lv_style_selector_t s){if(!same(o,LV_STYLE_ARC_COLOR,s,v))lv_obj_set_style_arc_color(o,v,s);}
void setWidth(lv_obj_t* o,int32_t v){if(!same(o,LV_STYLE_WIDTH,0,v))lv_obj_set_width(o,v);}
// Same palette as the home screen and status bar.
constexpr uint32_t kBg = 0x021018, kPanel = 0x0a2230, kBorder = 0x103d51, kText = 0xeaf4ff, kMuted = 0x9bbbd1;
constexpr uint32_t kAccent = 0x4bffc4, kWarn = 0xffbf77;
constexpr int kRows = kSettingsRows;  // settings list
constexpr int kCells = 9;  // menu: a 3 x 3 page of icons
constexpr unsigned kDrawRows = 20;
struct Badge { const char* symbol; uint32_t color; };
// Pages that draw their own title and body (no hero card, heading or notice line).
constexpr bool ownPage(App app){return app==App::Quake||app==App::Quiz||app==App::Hacker||app==App::Wifi||app==App::Sound||app==App::Motion||app==App::Forecast||app==App::Noise||app==App::Alarm||app==App::Stock||app==App::Typhoon||app==App::Hot||app==App::Memo||app==App::Dictation||app==App::Wheel||app==App::Translate||app==App::Guard||app==App::Todo||app==App::Ledger||app==App::Calc||app==App::Voice;}
// Once these pages show content, a refresh or the next item loads behind it instead of under the loading screen.
bool keepsContent(App app,const Model& m){
    return (app==App::Podcast&&m.podcastCount)||(app==App::Daily&&*m.page.daily.en)||(app==App::Quiz&&m.quizCount)||(app==App::Quake&&m.quakeCount)||(app==App::Hacker&&m.storyCount)||(app==App::Forecast&&m.page.forecast.days)||(app==App::Stock&&m.stockCount)||(app==App::Typhoon&&m.page.typhoon.count)||(app==App::Hot&&m.hotCount)||app==App::Memo||app==App::Translate||app==App::Guard;
}
// The footer: 0 the 录音 row, 1 a note in the list, 2 an open note, 3 its delete asked, 4 listening, 5 recognising.
unsigned memoTab(const Model& m){return m.listening()?4:m.voiceBusy()?5:m.memoArmed?3:m.memoDetail?2:m.memoIndex?1:0;}
unsigned dictationTab(const Model& m){return m.listening()?1:m.voiceBusy()?2:0;}
// 汇率, 自选股 and 倒数日: 0 the page, 1 listening, 2 recognising, 3 a delete asked.
unsigned voiceTab(const Model& m,bool armed){return m.voiceBusy()?dictationTab(m):armed?3:0;}
Badge appBadge(App app){
    switch(app){
    case App::Chat: return {ICON_ROBOT,0x5fd3ff};
    case App::Dictation: return {LV_SYMBOL_KEYBOARD,0xb59cff};
    case App::Codex: return {LV_SYMBOL_LIST,0x4bffc4};
    case App::Focus: return {LV_SYMBOL_BELL,0xff8a73};
    case App::Monitor: return {LV_SYMBOL_EYE_OPEN,0x7fb6ff};
    case App::Radio: return {LV_SYMBOL_VOLUME_MAX,0xffc861};
    case App::Podcast: return {LV_SYMBOL_AUDIO,0xff8a73};
    case App::Words: return {LV_SYMBOL_EDIT,0x4bffc4};
    case App::Slides: return {LV_SYMBOL_PLAY,0x91c892};
    case App::Alert: return {LV_SYMBOL_WARNING,0xff5a5a};
    case App::Status: return {LV_SYMBOL_FILE,0xf472b6};
    case App::Calendar: return {LV_SYMBOL_LIST,0xff8a73};
    case App::Countdown: return {LV_SYMBOL_LOOP,0x5fd3ff};
    case App::Torch: return {ICON_TORCH,0xffe066};
    case App::Qr: return {LV_SYMBOL_IMAGE,0x91c892};
    case App::News: return {LV_SYMBOL_UP,0xff6b6b};
    case App::Sonar: return {LV_SYMBOL_BLUETOOTH,0x4bd6ff};
    case App::Rate: return {"$",0xeab308};
    case App::Stopwatch: return {LV_SYMBOL_PAUSE,0xa3e635};
    case App::History: return {LV_SYMBOL_DIRECTORY,0xc4a1ff};
    case App::Daily: return {"Aa",0x5fd3ff};
    case App::Quake: return {LV_SYMBOL_GPS,0xff8a3d};
    case App::Quiz: return {"?",0xfbbf24};
    case App::Hacker: return {"Y",0xff6600};
    case App::Wifi: return {LV_SYMBOL_WIFI,0x5fd3ff};
    case App::Sound: return {LV_SYMBOL_VOLUME_MID,0xa78bfa};
    case App::Motion: return {LV_SYMBOL_EYE_OPEN,0x34d399};
    case App::Forecast: return {ICON_WEATHER,0x7fb6ff};
    case App::Alarm: return {ICON_CLOCK,0xffb454};
    case App::Noise: return {ICON_WIND,0x8fd3ff};
    case App::Stock: return {ICON_CHART,0xf04848};
    case App::Typhoon: return {ICON_TYPHOON,0x4bd6ff};
    case App::Hot: return {ICON_FIRE,0xff6a3c};
    case App::Memo: return {ICON_MIC,0x5eead4};
    case App::Wheel: return {LV_SYMBOL_SHUFFLE,0xffa94d};
    case App::Translate: return {"A",0x60a5fa};
    case App::Guard: return {LV_SYMBOL_EYE_CLOSE,0x22c55e};
    case App::Todo: return {LV_SYMBOL_OK,0xf5a524};
    case App::Ledger: return {ICON_WALLET,0x34d399};
    case App::Calc: return {ICON_CALC,0xfbbf24};
    case App::Voice: return {ICON_MIC,0x5fd3ff};
    default: return {LV_SYMBOL_SETTINGS,0x9bbbd1};
    }
}
Badge settingsBadge(SettingsPage page){
    switch(page){
    case SettingsPage::Wifi: return {LV_SYMBOL_WIFI,0x5fd3ff};
    case SettingsPage::Bluetooth: return {LV_SYMBOL_BLUETOOTH,0x7fb6ff};
    case SettingsPage::Brightness: return {LV_SYMBOL_EYE_OPEN,0xffc861};
    case SettingsPage::Volume: return {LV_SYMBOL_VOLUME_MAX,0x4bffc4};
    case SettingsPage::Wake: return {LV_SYMBOL_AUDIO,0xc792ff};
    case SettingsPage::City: return {LV_SYMBOL_GPS,0x34d399};
    default: return {LV_SYMBOL_POWER,0xff8a73};
    }
}
lv_display_t* display;
void* draw;
std::atomic<unsigned> flushPixels{},flushMicros{};
bool capturing;
std::atomic<unsigned> uiFree{},uiPeak{};
std::atomic<size_t> poolBytes{uiPoolCapacity(App::Home)};
#ifdef PASSPORT_LVGL_HEAP
UiPoolBuffer uiPool;
#endif
App guiApp{App::Home};bool resourcesReady=true;
void setLabel(lv_obj_t* obj,const char* value){if(obj&&strcmp(lv_label_get_text(obj),value))lv_label_set_text(obj,value);}
void trackUiMemory(){lv_mem_monitor_t m{};lv_mem_monitor(&m);uiFree=m.free_size;uiPeak=std::max(uiPeak.load(),static_cast<unsigned>(m.max_used));}
lv_obj_t *heading, *footer, *notice, *value, *sleepOverlay;
lv_obj_t *chatRoot,*chatLabels[6];
lv_obj_t *topBar,*footerRoot;
bool footerScrolling{};
lv_obj_t *rows[kRows], *rowLabels[kRows], *rowBadges[kRows], *rowIcons[kRows], *rowValues[kRows], *lines[4];
lv_obj_t *headingMark,*hero,*heroBadge,*heroIcon,*heroTrack,*heroFill,*detail;
lv_obj_t *pomoRoot,*pomoArc,*pomoTime,*pomoState,*pomoPhase,*pomoRounds,*pomoAction,*pomoActionIcon,*pomoActionText,*pomoDots[4];
lv_obj_t *pomoTabs[3],*pomoTabText[3];
lv_obj_t *dashboard,*dashboardTitle,*cards[2],*cardTitles[2],*cardValues[2],*cardDetails[2],*cardBars[2],*cardTrends[2],*dashboardFoot;
lv_obj_t *quotaNames[4],*quotaResets[4],*quotaValues[4],*quotaFills[4];App dashboardApp{};
lv_point_precise_t trendPoints[24];
lv_obj_t *tileTitles[4],*tileValues[4],*tileBars[4];
lv_obj_t *connectionRoot,*connectionTitle,*connectionSpinner,*connectionMessage,*connectionHint,*connectionError;
lv_obj_t *mediaRoot,*mediaTitle,*mediaTitleText,*mediaPosition,*mediaSleep,*mediaArt,*mediaArtIcon,*mediaName,*mediaMeta,*mediaStatus,*mediaDetail;
lv_obj_t *mediaVolume,*mediaDial,*mediaNeedle,*mediaNext,*mediaNote;
lv_obj_t *wordsRoot,*wordsPosition,*wordsTag,*wordsWord,*wordsIpa,*wordsMeaning,*wordsStatus,*wordsStats;
lv_obj_t *slidesRoot,*slidesLink,*slidesTime,*slidesCaption,*slidesStatus,*slidesLast,*slidesName;
lv_obj_t *alertRoot,*alertName;
lv_obj_t *statusRoot,*statusText,*statusTag,*statusFoot,*statusVoice;
lv_obj_t *cells[kCells],*cellBadges[kCells],*cellIcons[kCells],*cellNames[kCells],*menuTitle,*menuPage;
constexpr int kPodcastRows=5;
lv_obj_t *podRoot,*podPosition,*podSleep,*podList,*podRows[kPodcastRows],*podNames[kPodcastRows],*podNotes[kPodcastRows],*podPlayer,*podTitle,*podProgram,*podStatus,*podVolume,*podDetail,*podFill,*podTime,*podNote;
lv_obj_t *torchRoot,*torchName;
lv_obj_t *qrRoot,*qrCode,*qrTitleLabel,*qrInfo;uint32_t qrShown;
lv_obj_t *calendarRoot,*calendarTitle,*calendarYearName,*calendarGrid;
lv_obj_t *countRoot,*countName,*countDays,*countUnit,*countDate,*countInfo,*countList,*countVoice;
lv_obj_t *newsRoot,*newsQuotes,*newsMeta,*newsTitle,*newsDigest,*newsLines;
const char* lineEnd(const char* s,int room,const lv_font_t* font=LV_FONT_DEFAULT);
lv_obj_t *sonarRoot,*sonarView,*sonarMode,*sonarInfo,*sonarDetail;
lv_obj_t *swRoot,*swTime,*swState,*swLaps;
lv_obj_t *histRoot,*histMeta,*histYear,*histKind,*histTitle,*histNext[3];
lv_obj_t *dailyRoot,*dailyMeta,*dailyEnText,*dailyCnText;
lv_obj_t *calDetail,*calDate,*calLunar,*calGanzhi,*calChong,*calYi,*calJi,*calMoon;
lv_obj_t *rateRoot,*rateHead,*rateRows[RateBoard::kTargets],*rateNames[RateBoard::kTargets],*rateValues[RateBoard::kTargets],*rateFoot,*rateVoice;
const Model* drawModel;  // the model behind the custom-drawn calendar and sonar
uint32_t alertPanel;  // picture on the panel: colour | named<<24; UINT32_MAX = unknown
void box(lv_obj_t* obj, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    setBgColor(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, int w, const lv_font_t* font, uint32_t color) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y); setWidth(obj, w);
    setTextFont(obj, font, 0);
    setTextColor(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    return obj;
}
void visible(lv_obj_t* obj, bool show) {
    if(!obj)return;
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
// Short accent bar before every page title.
lv_obj_t* titleMark(lv_obj_t* parent,int x,int y){auto* mark=lv_obj_create(parent);box(mark,x,y,4,14,kAccent,2);return mark;}
void setBadge(lv_obj_t* badge,lv_obj_t* icon,Badge value,bool inverted){
    setBgColor(badge,lv_color_hex(inverted?kBg:value.color),0);
    setTextColor(icon,lv_color_hex(inverted?value.color:kBg),0);setLabel(icon,value.symbol);
}
void initChrome(lv_obj_t* screen){
    topBar=statusBarInit(screen);
    footerRoot=lv_obj_create(screen);box(footerRoot,0,292,240,28,0x021018);
    lv_obj_set_style_border_side(footerRoot,LV_BORDER_SIDE_TOP,0);lv_obj_set_style_border_width(footerRoot,1,0);lv_obj_set_style_border_color(footerRoot,lv_color_hex(0x103d51),0);
    footer=label(footerRoot,9,4,222,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(footer,20);
    lv_label_set_long_mode(footer,LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_anim_duration(footer,lv_anim_speed_clamped(30,800,20000),0);
}
void renderChrome(const Runtime& runtime){
    statusBarRender(runtime);const auto& m=runtime.model;
    const auto app=runtime.active();
    const bool alt=app==App::Calendar?m.calendarView!=0:app==App::Quiz?m.quizShown:app==App::Podcast?m.podcastPlayer:app==App::Stock?m.stockDetail:app==App::Hot?m.hotDetail:app==App::Forecast?m.forecastView==3&&m.page.forecast.warnings&&m.page.forecast.warnings!=0xff:app==App::Alarm?m.alarmRinging:app==App::Noise?m.noiseRow==std::size(kNoiseSounds):app==App::Stopwatch&&m.swRunning;
    const auto* hint=keyHint(app,m.settingsPage,m.focusRunning,*m.pairingCode,m.live&&m.livePhase==LivePhase::Loading,m.live&&m.livePhase==LivePhase::Error,alt,app==App::Wifi?m.wifiTab:app==App::Settings?(m.settingsPage==SettingsPage::City?dictationTab(m):m.settingsPage==SettingsPage::Idle?m.idleRow:m.wifiSetup?0u:m.wifiArmed?3u:m.wifiRow?2u:1u):
        app==App::Rate?voiceTab(m,m.rateArmed):app==App::Stock?voiceTab(m,m.stockArmed):app==App::Countdown?voiceTab(m,m.countdownArmed):
        app==App::Alarm?(m.listening()?3u:m.voiceBusy()?4u:m.alarmEdit?5u:m.alarmArmed?2u:m.alarmIndex?1u:0u):app==App::Memo?memoTab(m):app==App::Wheel?(m.wheelArmed?3u:dictationTab(m)):app==App::Todo?(m.todoRinging?5u:m.listening()?3u:m.voiceBusy()?4u:m.todoArmed?2u:m.todoIndex?1u:0u):app==App::Ledger?(m.listening()?3u:m.voiceBusy()?4u:m.ledgerArmed?2u:m.ledgerIndex?1u:0u):app==App::Calc||app==App::Voice?(m.listening()?1u:m.voiceBusy()?2u:0u):app==App::Dictation||app==App::Status||app==App::Translate?dictationTab(m):app==App::Calendar?m.calendarView:m.soundView);
    char text[256];
    // 打开 Mac 程序: what the computer is doing takes the key hints' place for a few seconds, on any page.
    if(*m.macNote)snprintf(text,sizeof(text),"%s",m.macNote);
    else if(runtime.active()==App::Words&&!*m.pairingCode&&!m.wordSession.size)snprintf(text,sizeof(text),"确认：再学 10 个  |  长按确认：返回");
    else snprintf(text,sizeof(text),"%s",hint);
    if(strcmp(lv_label_get_text(footer),text)){
        setLabel(footer,text);setTextColor(footer,lv_color_hex(*m.macNote?kAccent:kMuted),0);lv_point_t size{};
        lv_text_get_size(&size,text,LV_FONT_DEFAULT,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        footerScrolling=size.x>222;
    }
}
// Pomodoro: phase switch, a progress ring with the time, the next action, and dots for the
// rounds towards the long break. No rotated decorations: those cost a layer per frame.
void initPomodoro(lv_obj_t* screen) {
    pomoRoot=lv_obj_create(screen);box(pomoRoot,0,35,240,257,kBg);
    titleMark(pomoRoot,12,10);auto* title=label(pomoRoot,24,7,80,LV_FONT_DEFAULT,kText);setLabel(title,"番茄钟");
    pomoRounds=label(pomoRoot,96,7,70,LV_FONT_DEFAULT,kMuted);setTextAlign(pomoRounds,LV_TEXT_ALIGN_RIGHT,0);
    for(unsigned i=0;i<4;++i){pomoDots[i]=lv_obj_create(pomoRoot);box(pomoDots[i],174+i*14,12,9,9,kBorder,5);}
    auto* track=lv_obj_create(pomoRoot);box(track,12,32,216,26,kPanel,13);
    const char* names[]={"专注","短休","长休"};
    for(unsigned i=0;i<3;++i){pomoTabs[i]=lv_obj_create(track);box(pomoTabs[i],i*72,0,72,26,kPanel,13);pomoTabText[i]=label(pomoTabs[i],0,3,72,LV_FONT_DEFAULT,kMuted);setTextAlign(pomoTabText[i],LV_TEXT_ALIGN_CENTER,0);setLabel(pomoTabText[i],names[i]);}
    pomoArc=lv_arc_create(pomoRoot);lv_obj_set_size(pomoArc,158,158);lv_obj_set_pos(pomoArc,41,65);lv_arc_set_rotation(pomoArc,270);lv_arc_set_bg_angles(pomoArc,0,360);lv_arc_set_range(pomoArc,0,1000);lv_obj_remove_flag(pomoArc,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_style(pomoArc,nullptr,LV_PART_KNOB);
    lv_obj_set_style_arc_width(pomoArc,10,LV_PART_MAIN);lv_obj_set_style_arc_width(pomoArc,10,LV_PART_INDICATOR);lv_obj_set_style_arc_rounded(pomoArc,true,LV_PART_INDICATOR);setArcColor(pomoArc,lv_color_hex(kPanel),LV_PART_MAIN);
    pomoPhase=label(pomoRoot,60,98,120,LV_FONT_DEFAULT,kMuted);setTextAlign(pomoPhase,LV_TEXT_ALIGN_CENTER,0);
    pomoTime=label(pomoRoot,40,119,160,&lv_font_montserrat_48,kText);setTextAlign(pomoTime,LV_TEXT_ALIGN_CENTER,0);
    pomoState=label(pomoRoot,60,170,120,LV_FONT_DEFAULT,kMuted);setTextAlign(pomoState,LV_TEXT_ALIGN_CENTER,0);
    pomoAction=lv_obj_create(pomoRoot);box(pomoAction,40,229,160,26,0xed7b6a,13);
    pomoActionIcon=label(pomoAction,0,6,16,&lv_font_montserrat_14,kBg);pomoActionText=label(pomoAction,0,3,120,LV_FONT_DEFAULT,kBg);
    visible(pomoRoot,false);
}
void renderPomodoro(const Model& model) {
    const uint32_t colors[]={0xed7b6a,0x91c892,0x85b5df};const auto accent=colors[model.focusPhase];
    const char* names[]={"专注","短休","长休"};
    for(unsigned i=0;i<3;++i){setBgColor(pomoTabs[i],lv_color_hex(i==model.focusPhase?accent:kPanel),0);setTextColor(pomoTabText[i],lv_color_hex(i==model.focusPhase?kBg:kMuted),0);}
    const bool paused=!model.focusRunning&&!model.focusFinished&&model.focusRemaining<model.focusDuration;
    // Idle and finished show a full ring; a running or paused round shows the time left.
    setArcColor(pomoArc,lv_color_hex(accent),LV_PART_INDICATOR);
    lv_arc_set_value(pomoArc,model.focusRunning||paused?static_cast<int>(model.focusRemaining*1000/std::max<uint64_t>(model.focusDuration,1)):1000);
    char text[64];const bool hours=model.focusRemaining>=6000;const auto unit=hours?60ULL:1ULL;
    snprintf(text,sizeof(text),"%02llu:%02llu",model.focusRemaining/(unit*60),(model.focusRemaining/unit)%60);setLabel(pomoTime,text);
    setTextColor(pomoTime,lv_color_hex(paused?kMuted:kText),0);
    snprintf(text,sizeof(text),"%s · %llu 分钟%s",names[model.focusPhase],model.focusDuration/60,hours?" · 时:分":"");setLabel(pomoPhase,text);
    setLabel(pomoState,model.focusFinished?"● 本轮完成":model.focusRunning?(model.focusPhase?"● 休息中":"● 专注中"):paused?"● 已暂停":"● 准备开始");
    setTextColor(pomoState,lv_color_hex(model.focusRunning||model.focusFinished?accent:kMuted),0);
    // Dots fill towards the long break after every fourth focus round.
    unsigned dots=model.focusRounds%4;if(!dots&&model.focusRounds&&(model.focusPhase==2||(model.focusFinished&&model.focusPhase==0)))dots=4;
    for(unsigned i=0;i<4;++i)setBgColor(pomoDots[i],lv_color_hex(i<dots?colors[0]:kBorder),0);
    snprintf(text,sizeof(text),"完成 %lu 轮",static_cast<unsigned long>(model.focusRounds));setLabel(pomoRounds,text);
    const char* action=model.focusRunning?"暂停":paused?"继续":model.focusFinished?(model.focusPhase?"开始专注":model.focusRounds%4==0?"开始长休":"开始短休"):model.focusPhase?"开始休息":"开始专注";
    setBgColor(pomoAction,lv_color_hex(accent),0);setLabel(pomoActionIcon,model.focusRunning?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
    if(strcmp(lv_label_get_text(pomoActionText),action)){
        // Icon and text centred together in the button.
        lv_point_t size;lv_text_get_size(&size,action,LV_FONT_DEFAULT,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        const int left=(160-(14+6+size.x))/2;
        lv_obj_set_x(pomoActionIcon,left);lv_obj_set_x(pomoActionText,left+20);setLabel(pomoActionText,action);
    }
}
void initDashboard(lv_obj_t* screen,App app) {
    dashboard=lv_obj_create(screen);box(dashboard,0,35,240,257,kBg);dashboardApp=app;
    titleMark(dashboard,12,10);dashboardTitle=label(dashboard,24,7,204,LV_FONT_DEFAULT,kText);
    for(unsigned i=0;i<2;++i){
        cards[i]=lv_obj_create(dashboard);box(cards[i],12,32+i*101,216,95,kPanel,12);lv_obj_set_style_border_width(cards[i],1,0);lv_obj_set_style_border_color(cards[i],lv_color_hex(kBorder),0);
        if(app==App::Codex){
            // AI quota: one card per tool, a row per limit window with a remaining-share bar.
            cardTitles[i]=label(cards[i],12,6,192,LV_FONT_DEFAULT,kText);
            for(unsigned r=0;r<2;++r){
                const unsigned n=i*2+r;const int y=31+r*32;
                quotaNames[n]=label(cards[i],12,y,64,LV_FONT_DEFAULT,kText);lv_obj_set_height(quotaNames[n],20);
                quotaResets[n]=label(cards[i],58,y,96,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(quotaResets[n],20);
                quotaValues[n]=label(cards[i],150,y-2,54,&lv_font_montserrat_20,kAccent);setTextAlign(quotaValues[n],LV_TEXT_ALIGN_RIGHT,0);
                auto* track=lv_obj_create(cards[i]);box(track,12,y+22,192,4,kBorder,2);
                quotaFills[n]=lv_obj_create(track);box(quotaFills[n],0,0,1,4,kAccent,2);
            }
            continue;
        }
        break;
    }
    if(app==App::Monitor){
        // Computer status: a CPU card with its trend, then memory, disk, battery and network tiles.
        lv_obj_set_height(cards[0],82);
        cardTitles[0]=label(cards[0],12,6,80,LV_FONT_DEFAULT,kMuted);
        cardDetails[0]=label(cards[0],88,6,116,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(cardDetails[0],20);setTextAlign(cardDetails[0],LV_TEXT_ALIGN_RIGHT,0);
        cardValues[0]=label(cards[0],10,24,132,&lv_font_montserrat_48,kText);
        cardBars[0]=lv_obj_create(cards[0]);box(cardBars[0],12,74,192,3,kAccent,1);
        cardTrends[0]=lv_line_create(cards[0]);lv_obj_set_pos(cardTrends[0],143,32);lv_obj_set_style_line_width(cardTrends[0],2,0);lv_obj_set_style_line_color(cardTrends[0],lv_color_hex(kAccent),0);
        for(unsigned n=0;n<4;++n){
            auto* tile=lv_obj_create(dashboard);box(tile,12+n%2*110,120+n/2*58,106,54,kPanel,10);lv_obj_set_style_border_width(tile,1,0);lv_obj_set_style_border_color(tile,lv_color_hex(kBorder),0);
            tileTitles[n]=label(tile,8,3,92,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(tileTitles[n],20);
            tileValues[n]=label(tile,8,n==3?21:22,92,n==3?&lv_font_montserrat_14:&lv_font_montserrat_24,kText);
            tileBars[n]=lv_obj_create(tile);box(tileBars[n],8,48,1,3,kAccent,1);
        }
    }
    dashboardFoot=label(dashboard,12,app==App::Codex?232:234,216,LV_FONT_DEFAULT,kMuted);visible(dashboard,false);
}
// Chat page: the text record fills the page bottom-up; older entries clip off the top.
// It lives in the 12 KiB chat pool, so entries share two styles and point at the model's text.
lv_style_t chatUser,chatReply;uint32_t chatShown=UINT32_MAX;
void initChat(lv_obj_t* screen){
    for(auto* style:{&chatUser,&chatReply}){
        const bool user=style==&chatUser;lv_style_init(style);
        lv_style_set_radius(style,8);lv_style_set_pad_hor(style,8);lv_style_set_pad_ver(style,4);lv_style_set_bg_opa(style,LV_OPA_COVER);
        lv_style_set_bg_color(style,lv_color_hex(user?kAccent:kPanel));lv_style_set_text_color(style,lv_color_hex(user?kBg:kText));
        lv_style_set_text_align(style,user?LV_TEXT_ALIGN_RIGHT:LV_TEXT_ALIGN_LEFT);
    }
    chatRoot=lv_obj_create(screen);box(chatRoot,8,68,224,196,kBg);
    lv_obj_set_flex_flow(chatRoot,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(chatRoot,LV_FLEX_ALIGN_END,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(chatRoot,6,0);lv_obj_remove_flag(chatRoot,LV_OBJ_FLAG_SCROLLABLE);
    for(auto*& entry:chatLabels){entry=lv_label_create(chatRoot);setWidth(entry,224);lv_label_set_text_static(entry,"");}
    chatShown=UINT32_MAX;
}
void renderChat(const Model& model){
    if(chatShown==model.chatRevision)return;  // restyling every frame would relayout the whole record
    chatShown=model.chatRevision;
    for(unsigned i=0;i<6;++i){
        const bool shown=i<model.chatCount,hint=!i&&!model.chatCount;
        visible(chatLabels[i],shown||hint);lv_obj_remove_style_all(chatLabels[i]);setWidth(chatLabels[i],224);
        if(hint){setTextColor(chatLabels[i],lv_color_hex(kMuted),0);setTextAlign(chatLabels[i],LV_TEXT_ALIGN_CENTER,0);}
        else if(shown)lv_obj_add_style(chatLabels[i],model.chat[i].user?&chatUser:&chatReply,0);
        // Static text is re-read on every call, so an appended sentence shows without a copy.
        lv_label_set_text_static(chatLabels[i],shown?model.chat[i].text:hint?"说话后，对话文字会显示在这里":"");
    }
}
void initConnection(lv_obj_t* screen){
    connectionRoot=lv_obj_create(screen);box(connectionRoot,0,35,240,257,kBg);
    titleMark(connectionRoot,12,10);connectionTitle=label(connectionRoot,24,7,204,LV_FONT_DEFAULT,kText);
    connectionSpinner=lv_arc_create(connectionRoot);lv_obj_set_pos(connectionSpinner,88,55);lv_obj_set_size(connectionSpinner,64,64);lv_arc_set_bg_angles(connectionSpinner,0,360);lv_arc_set_range(connectionSpinner,0,100);lv_arc_set_value(connectionSpinner,30);
    lv_obj_remove_style(connectionSpinner,nullptr,LV_PART_KNOB);lv_obj_remove_flag(connectionSpinner,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_arc_width(connectionSpinner,5,LV_PART_MAIN);lv_obj_set_style_arc_width(connectionSpinner,5,LV_PART_INDICATOR);setArcColor(connectionSpinner,lv_color_hex(kPanel),LV_PART_MAIN);setArcColor(connectionSpinner,lv_color_hex(kAccent),LV_PART_INDICATOR);
    connectionError=label(connectionRoot,90,62,60,&lv_font_montserrat_48,kWarn);setLabel(connectionError,"!");setTextAlign(connectionError,LV_TEXT_ALIGN_CENTER,0);
    connectionMessage=label(connectionRoot,12,140,216,LV_FONT_DEFAULT,kText);setTextAlign(connectionMessage,LV_TEXT_ALIGN_CENTER,0);
    connectionHint=label(connectionRoot,18,178,204,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(connectionHint,60);lv_label_set_long_mode(connectionHint,LV_LABEL_LONG_WRAP);setTextAlign(connectionHint,LV_TEXT_ALIGN_CENTER,0);
    visible(connectionRoot,false);
}
// 定时关闭 left, beside the page's chip; empty when no timer runs.
void setSleepLabel(lv_obj_t* l,const Model& m){
    char text[24]{};const uint64_t now=esp_timer_get_time()/1000000;
    if(m.mediaUntil>now){const unsigned left=m.mediaUntil-now;snprintf(text,sizeof(text),"定时 %u:%02u",left/60,left%60);}
    setLabel(l,text);
}
// Radio: a tuning dial with one mark per station.
void initMedia(lv_obj_t* screen){
    mediaRoot=lv_obj_create(screen);box(mediaRoot,0,35,240,257,kBg);
    mediaTitle=lv_obj_create(mediaRoot);box(mediaTitle,8,4,70,24,kAccent,12);mediaTitleText=label(mediaTitle,0,2,70,LV_FONT_DEFAULT,kBg);setTextAlign(mediaTitleText,LV_TEXT_ALIGN_CENTER,0);setLabel(mediaTitleText,"收音机");
    mediaPosition=label(mediaRoot,160,8,72,&lv_font_montserrat_14,kMuted);setTextAlign(mediaPosition,LV_TEXT_ALIGN_RIGHT,0);
    mediaSleep=label(mediaRoot,86,6,90,LV_FONT_DEFAULT,kAccent);
    auto* card=lv_obj_create(mediaRoot);box(card,8,34,224,138,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    mediaArt=lv_obj_create(card);box(mediaArt,12,12,56,56,kWarn,12);
    mediaArtIcon=label(mediaArt,0,16,56,&lv_font_montserrat_24,kBg);setTextAlign(mediaArtIcon,LV_TEXT_ALIGN_CENTER,0);
    mediaName=label(card,78,10,134,LV_FONT_DEFAULT,kText);lv_obj_set_height(mediaName,42);
    mediaMeta=label(card,78,52,134,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(mediaMeta,20);
    mediaStatus=label(card,12,76,140,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(mediaStatus,20);
    mediaVolume=label(card,150,79,62,&lv_font_montserrat_14,kMuted);setTextAlign(mediaVolume,LV_TEXT_ALIGN_RIGHT,0);
    mediaDetail=label(card,12,98,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(mediaDetail,40);
    auto* lower=lv_obj_create(mediaRoot);box(lower,8,178,224,56,kPanel,12);lv_obj_set_style_border_width(lower,1,0);lv_obj_set_style_border_color(lower,lv_color_hex(kBorder),0);
    mediaDial=lv_scale_create(lower);lv_obj_remove_style_all(mediaDial);lv_obj_set_pos(mediaDial,16,2);lv_obj_set_size(mediaDial,192,12);
    lv_scale_set_mode(mediaDial,LV_SCALE_MODE_HORIZONTAL_BOTTOM);lv_scale_set_label_show(mediaDial,true);
    lv_obj_set_style_length(mediaDial,9,LV_PART_INDICATOR);lv_obj_set_style_length(mediaDial,4,LV_PART_ITEMS);
    lv_obj_set_style_line_width(mediaDial,2,LV_PART_INDICATOR);lv_obj_set_style_line_width(mediaDial,1,LV_PART_ITEMS);lv_obj_set_style_line_width(mediaDial,1,LV_PART_MAIN);
    lv_obj_set_style_line_color(mediaDial,lv_color_hex(kMuted),LV_PART_INDICATOR);lv_obj_set_style_line_color(mediaDial,lv_color_hex(kBorder),LV_PART_ITEMS);lv_obj_set_style_line_color(mediaDial,lv_color_hex(kBorder),LV_PART_MAIN);
    setTextFont(mediaDial,&lv_font_montserrat_14,LV_PART_INDICATOR);setTextColor(mediaDial,lv_color_hex(kMuted),LV_PART_INDICATOR);
    mediaNeedle=lv_obj_create(lower);box(mediaNeedle,0,0,3,30,0xff6b5f,1);
    mediaNext=label(lower,12,33,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(mediaNext,20);
    mediaNote=label(mediaRoot,12,236,216,LV_FONT_DEFAULT,kWarn);lv_obj_set_height(mediaNote,20);
}
void renderMedia(const Model& model){
    // Built-in sample until the device's first reply.
    const bool live=model.live;
    const unsigned count=live?model.mediaCount:3,index=live?model.mediaIndex:1;
    const char* name=live?model.liveValue:"示例电台";
    const char* meta=live?model.liveLines[0]:"本地 · 64k";
    const char* detail=live?model.liveLines[1]:"所在城市：未选择";
    const char* state=live?model.liveState:"已停止";
    char text[96];
    if(count)snprintf(text,sizeof(text),"%u / %u",index+1,count);else text[0]=0;
    setLabel(mediaPosition,text);setSleepLabel(mediaSleep,model);
    setBgColor(mediaArt,lv_color_hex(count?0xff8a73:kBorder),0);
    setLabel(mediaArtIcon,count?LV_SYMBOL_VOLUME_MAX:LV_SYMBOL_REFRESH);
    setLabel(mediaName,name);setLabel(mediaMeta,meta);setLabel(mediaDetail,detail);
    setLabel(mediaNext,live?model.liveLines[2]:"下一个：示例");setLabel(mediaNote,live?model.liveLines[3]:"");
    const bool playing=!strcmp(state,"正在播放");
    if(playing)snprintf(text,sizeof(text),"● LIVE · %s",state);else snprintf(text,sizeof(text),"● %s",state);
    setLabel(mediaStatus,text);
    snprintf(text,sizeof(text),LV_SYMBOL_VOLUME_MAX " %u%%",model.volume);setLabel(mediaVolume,text);
    setTextColor(mediaStatus,lv_color_hex(model.livePhase==LivePhase::Error&&live?kWarn:playing?kAccent:model.livePhase==LivePhase::Loading&&live?kWarn:kMuted),0);
    const bool dial=count>1;
    visible(mediaDial,dial);visible(mediaNeedle,dial);
    if(dial){
        // Numbered 1..count like a dial's frequencies: four minor ticks between few stations, one tick each and a number every fifth for many.
        const bool few=count<=8;const unsigned ticks=few?(count-1)*4+1:count;
        if(lv_scale_get_total_tick_count(mediaDial)!=ticks){lv_scale_set_total_tick_count(mediaDial,ticks);lv_scale_set_major_tick_every(mediaDial,few?4:5);lv_scale_set_range(mediaDial,1,count);}
        lv_obj_set_x(mediaNeedle,16+static_cast<int>(index*191/(count-1))-1);
    }
}
// 播客: the program list (five rows in view), and once one is opened the player with the episode's progress.
void initPodcast(lv_obj_t* screen){
    podRoot=lv_obj_create(screen);box(podRoot,0,35,240,257,kBg);
    auto* chip=lv_obj_create(podRoot);box(chip,8,4,70,24,kAccent,12);auto* chipText=label(chip,0,2,70,LV_FONT_DEFAULT,kBg);setTextAlign(chipText,LV_TEXT_ALIGN_CENTER,0);setLabel(chipText,"播客");
    podPosition=label(podRoot,120,8,112,&lv_font_montserrat_14,kMuted);setTextAlign(podPosition,LV_TEXT_ALIGN_RIGHT,0);
    podSleep=label(podRoot,86,6,90,LV_FONT_DEFAULT,kAccent);
    podList=lv_obj_create(podRoot);box(podList,0,34,240,220,kBg);
    for(int i=0;i<kPodcastRows;++i){
        podRows[i]=lv_obj_create(podList);box(podRows[i],8,i*44,224,42,kPanel,8);
        podNames[i]=label(podRows[i],10,1,204,LV_FONT_DEFAULT,kText);lv_obj_set_height(podNames[i],20);
        podNotes[i]=label(podRows[i],10,21,204,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(podNotes[i],20);
    }
    podPlayer=lv_obj_create(podRoot);box(podPlayer,0,34,240,223,kBg);
    auto* card=lv_obj_create(podPlayer);box(card,8,0,224,138,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    auto* art=lv_obj_create(card);box(art,12,12,56,56,0xff8a73,12);
    auto* artIcon=label(art,0,16,56,&lv_font_montserrat_24,kBg);setTextAlign(artIcon,LV_TEXT_ALIGN_CENTER,0);setLabel(artIcon,LV_SYMBOL_AUDIO);
    podTitle=label(card,78,10,134,LV_FONT_DEFAULT,kText);lv_obj_set_height(podTitle,42);lv_label_set_long_mode(podTitle,LV_LABEL_LONG_WRAP);lv_obj_set_style_max_height(podTitle,42,0);
    podProgram=label(card,78,52,134,LV_FONT_DEFAULT,kAccent);lv_obj_set_height(podProgram,20);
    podStatus=label(card,12,76,140,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(podStatus,20);
    podVolume=label(card,150,79,62,&lv_font_montserrat_14,kMuted);setTextAlign(podVolume,LV_TEXT_ALIGN_RIGHT,0);
    podDetail=label(card,12,100,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(podDetail,20);
    auto* lower=lv_obj_create(podPlayer);box(lower,8,144,224,56,kPanel,12);lv_obj_set_style_border_width(lower,1,0);lv_obj_set_style_border_color(lower,lv_color_hex(kBorder),0);
    auto* track=lv_obj_create(lower);box(track,16,12,192,6,kBorder,3);podFill=lv_obj_create(track);box(podFill,0,0,1,6,kAccent,3);
    podTime=label(lower,12,26,200,&lv_font_montserrat_14,kMuted);setTextAlign(podTime,LV_TEXT_ALIGN_CENTER,0);
    podNote=label(podPlayer,12,202,216,LV_FONT_DEFAULT,kWarn);lv_obj_set_height(podNote,20);
}
void renderPodcast(const Model& m){
    // Built-in sample until the device's first reply (the simulator has no device).
    static constexpr Model::PodcastRow kSample[]{{"声动早咖啡","从最新一期开始"},{"日谈公园","听到第 312 期 · 12:30"},{"BBC 随身英语","从最新一期开始"},{"TED 演讲精选","从最新一期开始"}};
    const bool live=m.live,player=m.podcastPlayer;
    const unsigned count=live?m.podcastCount:std::size(kSample),selected=count?m.podcastIndex%count:0;
    const auto row=[&](unsigned i)->const Model::PodcastRow&{return live?m.page.podcasts[i]:kSample[i];};
    char text[96];
    if(count)snprintf(text,sizeof(text),"%u / %u",selected+1,count);else text[0]=0;
    setLabel(podPosition,text);setSleepLabel(podSleep,m);visible(podList,!player);visible(podPlayer,player);
    if(!player){
        const unsigned first=count>kPodcastRows?std::min(selected>2?selected-2:0,count-kPodcastRows):0;
        for(unsigned i=0;i<kPodcastRows;++i){
            const unsigned index=first+i;visible(podRows[i],index<count);if(index>=count)continue;
            const bool on=index==selected;
            setBgColor(podRows[i],lv_color_hex(on?kAccent:kPanel),0);
            setLabel(podNames[i],row(index).name);setTextColor(podNames[i],lv_color_hex(on?kBg:kText),0);
            setLabel(podNotes[i],row(index).note);setTextColor(podNotes[i],lv_color_hex(on?kBg:kMuted),0);
        }
        return;
    }
    setLabel(podTitle,live?m.liveValue:"示例节目");setLabel(podProgram,live?m.liveLines[0]:row(selected).name);
    setLabel(podDetail,live?m.liveLines[1]:"");setLabel(podNote,live?m.liveLines[2]:"");
    snprintf(text,sizeof(text),"● %s",live?m.liveState:"已暂停");setLabel(podStatus,text);
    setTextColor(podStatus,lv_color_hex(m.livePhase==LivePhase::Error&&live?kWarn:m.mediaPlaying?kAccent:m.livePhase==LivePhase::Loading&&live?kWarn:kMuted),0);
    snprintf(text,sizeof(text),LV_SYMBOL_VOLUME_MAX " %u%%",m.volume);setLabel(podVolume,text);
    const auto clock=[](char* out,size_t size,uint32_t s){if(s>=3600)snprintf(out,size,"%lu:%02lu:%02lu",static_cast<unsigned long>(s/3600),static_cast<unsigned long>(s/60%60),static_cast<unsigned long>(s%60));else snprintf(out,size,"%02lu:%02lu",static_cast<unsigned long>(s/60),static_cast<unsigned long>(s%60));};
    char at[16],length[16];clock(at,sizeof(at),m.podcastPos);clock(length,sizeof(length),m.podcastDur);
    if(m.podcastDur)snprintf(text,sizeof(text),"%s / %s",at,length);else text[0]=0;setLabel(podTime,text);
    setWidth(podFill,std::max<int>(1,m.podcastDur?std::min<uint32_t>(m.podcastPos,m.podcastDur)*192/m.podcastDur:0));
}
// Word card: the word, its IPA and meaning; a review hides the meaning until Confirm reads the word aloud.
void initWords(lv_obj_t* screen){
    wordsRoot=lv_obj_create(screen);box(wordsRoot,0,35,240,257,kBg);
    auto* chip=lv_obj_create(wordsRoot);box(chip,8,4,70,24,kAccent,12);auto* chipText=label(chip,0,2,70,LV_FONT_DEFAULT,kBg);setTextAlign(chipText,LV_TEXT_ALIGN_CENTER,0);setLabel(chipText,"背单词");
    wordsPosition=label(wordsRoot,120,8,112,&lv_font_montserrat_14,kMuted);setTextAlign(wordsPosition,LV_TEXT_ALIGN_RIGHT,0);
    auto* card=lv_obj_create(wordsRoot);box(card,8,34,224,150,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    wordsTag=label(card,12,8,200,LV_FONT_DEFAULT,kAccent);lv_obj_set_height(wordsTag,20);
    wordsWord=label(card,12,32,200,&lv_font_montserrat_24,kText);
    wordsIpa=label(card,12,66,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(wordsIpa,20);
    auto* rule=lv_obj_create(card);box(rule,12,94,200,1,kBorder);
    wordsMeaning=label(card,12,102,200,LV_FONT_DEFAULT,kText);lv_obj_set_height(wordsMeaning,42);lv_label_set_long_mode(wordsMeaning,LV_LABEL_LONG_WRAP);
    auto* lower=lv_obj_create(wordsRoot);box(lower,8,190,224,58,kPanel,12);lv_obj_set_style_border_width(lower,1,0);lv_obj_set_style_border_color(lower,lv_color_hex(kBorder),0);
    wordsStatus=label(lower,12,6,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(wordsStatus,20);
    wordsStats=label(lower,12,30,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(wordsStats,20);
}
void renderWords(const Model& model){
    const auto& deck=model.words;const auto& session=model.wordSession;const auto* card=wordsCurrent(deck,session);char text[96];
    if(card)snprintf(text,sizeof(text),"%u / %u",session.cursor+1,session.size);else text[0]=0;
    setLabel(wordsPosition,text);
    if(!card){
        setLabel(wordsTag,"今日完成");setLabel(wordsWord,"Well done!");setLabel(wordsIpa,"");
        setTextFont(wordsWord,&lv_font_montserrat_24,0);setTextColor(wordsWord,lv_color_hex(kAccent),0);
        setTextColor(wordsMeaning,lv_color_hex(kMuted),0);setLabel(wordsMeaning,deck.next<kWordCount?"今天的单词都背完了，确认再学 10 个":"整个词库都学过了");
    }else{
        const char* word=wordAt(card->word);
        if(card->box)snprintf(text,sizeof(text),"复习 · 第 %u 轮",card->box);else snprintf(text,sizeof(text),"新词");
        setLabel(wordsTag,text);setTextColor(wordsTag,lv_color_hex(card->box?kWarn:kAccent),0);
        setTextFont(wordsWord,strlen(word)>12?&lv_font_montserrat_20:&lv_font_montserrat_24,0);setTextColor(wordsWord,lv_color_hex(kText),0);
        setLabel(wordsWord,word);
        // IPA and meaning are looked up online (LiveServices "look:N"); the reply names its word.
        const bool known=!strcmp(model.liveValue,word)&&*model.liveLines[1];
        if(known&&*model.liveLines[0])snprintf(text,sizeof(text),"/%.80s/",model.liveLines[0]);else text[0]=0;
        setLabel(wordsIpa,text);
        const bool shown=!card->box||session.revealed;
        setTextColor(wordsMeaning,lv_color_hex(shown&&known?kText:kMuted),0);
        setLabel(wordsMeaning,!shown?"想一想意思，确认键朗读并显示":known?model.liveLines[1]:model.livePhase==LivePhase::Error?"释义暂不可用":"正在查询释义…");
    }
    // Reading aloud is the only networked part; before the device's first reply it is idle.
    const bool live=model.live&&*model.liveState&&strcmp(model.liveState,"连接中");
    const bool error=live&&model.livePhase==LivePhase::Error,busy=live&&model.livePhase==LivePhase::Loading;
    snprintf(text,sizeof(text),"● %s",!card?"明天还有新词和复习":live?model.liveState:"按确认朗读");setLabel(wordsStatus,text);
    setTextColor(wordsStatus,lv_color_hex(error?kWarn:busy?kAccent:kMuted),0);
    snprintf(text,sizeof(text),"已学 %u / %u  ·  掌握 %u",deck.next,kWordCount,deck.mastered);setLabel(wordsStats,text);
}
// Warning light: one full-screen panel above the chrome; only its colour changes per frame.
void initAlert(lv_obj_t* screen){
    alertRoot=lv_obj_create(screen);box(alertRoot,0,0,240,320,0);
    alertPanel=UINT32_MAX;
    alertName=label(alertRoot,10,146,220,LV_FONT_DEFAULT,0xffffff);setTextAlign(alertName,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_bg_color(alertName,lv_color_hex(0),0);lv_obj_set_style_bg_opa(alertName,LV_OPA_COVER,0);
    lv_obj_set_style_radius(alertName,8,0);lv_obj_set_style_pad_ver(alertName,4,0);
}
// Returns true when it painted the screen itself and LVGL must not draw this frame.
bool renderAlert(const Model& m){
    const auto& mode=kAlertModes[m.alertMode];
    const auto now=esp_timer_get_time();
    const unsigned frame=now/125000%mode.count;
    // This frame's colour, or on a dark frame the next one to light up.
    unsigned ahead=0;while(ahead<mode.count&&!mode.colors[(frame+ahead)%mode.count])++ahead;
    const uint32_t want=mode.colors[(frame+ahead)%mode.count];
    const bool named=(now+ahead*125000)/1000000-m.alertChanged<2;
    char text[96];snprintf(text,sizeof(text),"%s\n上下切换 | 长按确认退出",mode.name);setLabel(alertName,text);
    visible(alertName,named);
#ifdef PASSPORT_DEVICE
    // The panel has no fill command: any new picture is a ~45-100 ms SPI write that shows as a
    // top-down wipe. So pictures are written with the backlight off (on dark frames) and lit
    // frames only switch it on. Without the name, one prepared band is sent down the screen.
    const uint32_t key=want|(named?1u<<24:0);
    const bool lit=mode.colors[frame];
    board::backlight(lit&&alertPanel==key);
    if(alertPanel!=key){
        if(named){setBgColor(alertRoot,lv_color_hex(want),0);lv_obj_invalidate(alertRoot);lv_timer_handler();lv_refr_now(display);}
        else{
            const auto started=esp_timer_get_time();
            board::fill(lv_color_to_u16(lv_color_hex(want)),static_cast<uint16_t*>(draw),kDrawRows);
            flushPixels+=240*320;flushMicros+=esp_timer_get_time()-started;
        }
        alertPanel=key;board::backlight(lit);
    }
    return true;
#else
    setBgColor(alertRoot,lv_color_hex(mode.colors[frame]),0);
    return false;
#endif
}
void initStatusVoice(lv_obj_t* screen);
// 倒数日 and 汇率 draw with labels: while a phrase is spoken and recognised this layer covers them (notes: the line
// under the listening and the recognising screen).
lv_obj_t* voiceLayer(lv_obj_t* parent,const char* const* notes);
// Status sign: one card in the label's colour, the label in 48 px, its tag, and the page's lit switch.
// While a spoken label is heard and recognised, 语音笔记's listening screen covers the card.
void initStatus(lv_obj_t* screen){
    statusRoot=lv_obj_create(screen);box(statusRoot,8,40,224,246,kPanel,16);
    statusText=label(statusRoot,8,0,208,&passport_status_font,kText);
    lv_label_set_long_mode(statusText,LV_LABEL_LONG_WRAP);setTextAlign(statusText,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_line_space(statusText,-8,0);lv_obj_set_style_max_height(statusText,176,0);
    lv_obj_align(statusText,LV_ALIGN_CENTER,0,-18);
    statusTag=label(statusRoot,8,188,208,&lv_font_montserrat_14,kText);setTextAlign(statusTag,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_letter_space(statusTag,2,0);lv_obj_set_style_text_opa(statusTag,LV_OPA_70,0);
    statusFoot=label(statusRoot,8,216,208,LV_FONT_DEFAULT,kText);setTextAlign(statusFoot,LV_TEXT_ALIGN_CENTER,0);
    initStatusVoice(screen);
}
void renderStatus(const Model& m){
    const auto entry=m.statusLabel(m.status.index);
    // Dark text on the light (amber, green) cards, white on the rest.
    const uint32_t c=entry.color,ink=((c>>16)*299+(c>>8&255)*587+(c&255)*114)/1000>150?kBg:0xffffff;
    setBgColor(statusRoot,lv_color_hex(c),0);
    // Typed labels use the enlarged font throughout: mixing it with the sharper preset glyphs looks uneven.
    setTextFont(statusText,*entry.tag?&passport_status_font:&passport_ui_font_x3,0);
    setLabel(statusText,entry.text);setTextColor(statusText,lv_color_hex(ink),0);
    setLabel(statusTag,*entry.tag?entry.tag:"MY STATUS");setTextColor(statusTag,lv_color_hex(ink),0);
    char text[48];snprintf(text,sizeof(text),"%u / %u  ·  %s",m.status.index+1,m.statusCount(),m.status.awake?"常亮":"自动熄屏");
    setLabel(statusFoot,*m.lastNotice?m.lastNotice:text);setTextColor(statusFoot,lv_color_hex(ink),0);
    visible(statusVoice,m.voiceBusy());visible(statusRoot,!m.voiceBusy());
    if(m.voiceBusy()){drawModel=&m;lv_obj_invalidate(statusVoice);}
}
// Menu: a themed page of 3 x 3 icons under its name and number; the selection moves through them in order and
// the page follows.
void initMenu(lv_obj_t* screen){
    titleMark(screen,12,43);menuTitle=label(screen,24,40,160,LV_FONT_DEFAULT,kText);
    menuPage=label(screen,170,42,58,&lv_font_montserrat_14,kMuted);setTextAlign(menuPage,LV_TEXT_ALIGN_RIGHT,0);
    for(int i=0;i<kCells;++i){
        cells[i]=lv_obj_create(screen);box(cells[i],8+i%3*76,64+i/3*76,72,72,kPanel,12);
        cellBadges[i]=lv_obj_create(cells[i]);box(cellBadges[i],16,6,40,40,kAccent,12);
        cellIcons[i]=label(cellBadges[i],0,8,40,&passport_icon_font_20,kBg);setTextAlign(cellIcons[i],LV_TEXT_ALIGN_CENTER,0);
        cellNames[i]=label(cells[i],0,48,72,LV_FONT_DEFAULT,kText);setTextAlign(cellNames[i],LV_TEXT_ALIGN_CENTER,0);
    }
}
void renderMenu(const Runtime& runtime){
    const unsigned selected=runtime.selection(),page=menuPageOf(selected),pages=std::size(kMenuPages);
    for(unsigned i=0;i<kCells;++i){
        const unsigned index=kMenuStarts[page]+i;visible(cells[i],index<kMenuStarts[page+1]);if(index>=kMenuStarts[page+1])continue;
        const auto app=kFunctions[index];const bool on=index==selected;auto badge=appBadge(app);
        // The calendar icon shows today's date.
        char day[4];const time_t now=time(nullptr);tm t{};localtime_r(&now,&t);
        if(app==App::Calendar&&runtime.model.clockValid){snprintf(day,sizeof(day),"%d",t.tm_mday);badge.symbol=day;}
        setBadge(cellBadges[i],cellIcons[i],badge,on);
        setBgColor(cells[i],lv_color_hex(on?kAccent:kPanel),0);
        setLabel(cellNames[i],menuLabel(app));setTextColor(cellNames[i],lv_color_hex(on?kBg:kText),0);
    }
    setLabel(menuTitle,kMenuPages[page]);
    char text[8];snprintf(text,sizeof(text),"%u/%u",page+1,pages);setLabel(menuPage,text);
}
// Torch: the whole screen in one colour; main.cpp drives the backlight from the level.
void initTorch(lv_obj_t* screen){
    torchRoot=lv_obj_create(screen);box(torchRoot,0,0,240,320,0xffffff);
    torchName=label(torchRoot,10,140,220,LV_FONT_DEFAULT,0xffffff);setTextAlign(torchName,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_bg_color(torchName,lv_color_hex(0),0);lv_obj_set_style_bg_opa(torchName,LV_OPA_70,0);
    lv_obj_set_style_radius(torchName,8,0);lv_obj_set_style_pad_ver(torchName,4,0);
}
void renderTorch(const Model& m){
    setBgColor(torchRoot,lv_color_hex(kTorchColors[m.torchColor]),0);
    char text[96];snprintf(text,sizeof(text),"%s · 亮度 %d%%\n上下亮度 | 确认颜色 | 长按确认退出",kTorchNames[m.torchColor],m.torchLevel*20);
    setLabel(torchName,text);visible(torchName,esp_timer_get_time()/1000000-m.torchChanged<2);
}
// QR page: the code on a white tile (its own quiet zone), the entry's title below.
void initQr(lv_obj_t* screen){
    qrRoot=lv_obj_create(screen);box(qrRoot,0,35,240,257,kBg);
    auto* tile=lv_obj_create(qrRoot);box(tile,20,6,200,200,0xffffff,12);
    qrCode=lv_qrcode_create(tile);lv_qrcode_set_size(qrCode,184);lv_obj_set_pos(qrCode,8,8);
    lv_qrcode_set_dark_color(qrCode,lv_color_hex(0));lv_qrcode_set_light_color(qrCode,lv_color_hex(0xffffff));
    qrTitleLabel=label(qrRoot,10,212,220,LV_FONT_DEFAULT,kText);setTextAlign(qrTitleLabel,LV_TEXT_ALIGN_CENTER,0);
    qrInfo=label(qrRoot,10,234,220,LV_FONT_DEFAULT,kMuted);setTextAlign(qrInfo,LV_TEXT_ALIGN_CENTER,0);
    qrShown=UINT32_MAX;
}
void renderQr(const Runtime& runtime){
    const auto& m=runtime.model;const unsigned count=runtime.qrCount();
    visible(qrCode,count&&*m.qrText);
    // Encoding is the slow part: redo it only when the text changes.
    uint32_t hash=2166136261u;for(const char* c=m.qrText;*c;++c)hash=(hash^static_cast<uint8_t>(*c))*16777619u;
    if(*m.qrText&&hash!=qrShown){qrShown=hash;lv_qrcode_update(qrCode,m.qrText,strlen(m.qrText));}
    setLabel(qrTitleLabel,count?m.qrTitle:"还没有二维码");
    char text[64];if(count)snprintf(text,sizeof(text),"%u / %u%s",m.qrIndex+1,count,m.qrBright?"  ·  已提亮":"");
    setLabel(qrInfo,count?text:"在电脑助手「设备内容」里添加");
}
// 万年历: one object draws the whole month (42 labels would not fit the UI pool).
void drawCalendar(lv_event_t* e){
    const auto& m=*drawModel;auto* layer=lv_event_get_layer(e);lv_area_t area;lv_obj_get_coords(calendarGrid,&area);
    const Date first{m.calendarYear,m.calendarMonth,1};const int lead=weekday(first),days=monthDays(first.year,first.month);
    const int rows=(lead+days+6)/7,h=(lv_area_get_height(&area)-18)/rows;
    int todayNumber=-1;{const time_t now=time(nullptr);tm t{};localtime_r(&now,&t);if(m.clockValid)todayNumber=dayNumber({t.tm_year+1900,t.tm_mon+1,t.tm_mday});}
    lv_draw_label_dsc_t text;lv_draw_label_dsc_init(&text);text.align=LV_TEXT_ALIGN_CENTER;text.text_local=1;
    static const char* const week[]{"日","一","二","三","四","五","六"};
    for(int c=0;c<7;++c){
        lv_area_t cell{area.x1+c*34,area.y1,area.x1+c*34+33,area.y1+17};
        text.font=LV_FONT_DEFAULT;text.color=lv_color_hex(c==0||c==6?0xff8a73:kMuted);text.text=week[c];lv_draw_label(layer,&text,&cell);
    }
    for(int d=1;d<=days;++d){
        const int at=lead+d-1,c=at%7,r=at/7;const Date date{first.year,first.month,d};
        lv_area_t cell{area.x1+c*34,area.y1+18+r*h,area.x1+c*34+33,area.y1+18+r*h+h-2};
        LunarDate lunar{};toLunar(date,lunar);const int off=holiday(date);const char* name=festival(date,lunar);
        const bool today=dayNumber(date)==todayNumber,chosen=d==m.calendarDay;
        if(chosen&&!today){
            lv_draw_rect_dsc_t ring;lv_draw_rect_dsc_init(&ring);ring.radius=6;ring.bg_opa=LV_OPA_TRANSP;ring.border_width=2;ring.border_color=lv_color_hex(kAccent);
            lv_draw_rect(layer,&ring,&cell);
        }
        if(today||off){
            lv_draw_rect_dsc_t back;lv_draw_rect_dsc_init(&back);back.radius=6;
            back.bg_color=lv_color_hex(today?kAccent:off==1?0x3a1d24:0x1d2a3a);lv_draw_rect(layer,&back,&cell);
        }
        char number[12];snprintf(number,sizeof(number),"%d",d);
        const uint32_t ink=today?kBg:off==1||c==0||c==6?0xff8a73:kText;
        lv_area_t top=cell;top.y2=top.y1+16;top.y1+=1;
        text.font=&lv_font_montserrat_14;text.color=lv_color_hex(off==2&&!today?kWarn:ink);text.text=number;lv_draw_label(layer,&text,&top);
        // Below the day: festival or term, the month name on the 1st, otherwise the lunar day; 班 marks a make-up working day.
        const char* below=off==2?"补班":name?name:lunar.day==1?lunarMonthName(lunar.month):lunarDayName(lunar.day);
        lv_area_t bottom=cell;bottom.y1+=h>=40?19:16;
        // A 34 px cell holds two characters: 劳动节 shows as 劳动.
        char shortName[7];snprintf(shortName,sizeof(shortName),"%s",below);
        text.font=LV_FONT_DEFAULT;text.color=lv_color_hex(today?kBg:off==2?kWarn:name?0xffc861:lunar.day==1?kAccent:kMuted);text.text=shortName;
        lv_draw_label(layer,&text,&bottom);
    }
}
void drawCalMoon(lv_event_t* e);
void initCalendar(lv_obj_t* screen){
    calendarRoot=lv_obj_create(screen);box(calendarRoot,0,35,240,257,kBg);
    calendarTitle=label(calendarRoot,12,4,120,LV_FONT_DEFAULT,kText);
    calendarYearName=label(calendarRoot,110,4,120,LV_FONT_DEFAULT,kMuted);setTextAlign(calendarYearName,LV_TEXT_ALIGN_RIGHT,0);
    calendarGrid=lv_obj_create(calendarRoot);box(calendarGrid,1,27,238,228,kBg);
    lv_obj_add_event_cb(calendarGrid,drawCalendar,LV_EVENT_DRAW_MAIN,nullptr);
    // The selected day's 黄历, computed on the badge (lunar.cpp; no almanac API is free without a key).
    calDetail=lv_obj_create(calendarRoot);box(calDetail,0,0,240,257,kBg);
    calDate=label(calDetail,12,2,216,LV_FONT_DEFAULT,kText);
    calLunar=label(calDetail,12,24,216,LV_FONT_DEFAULT,kAccent);
    calGanzhi=label(calDetail,12,46,216,LV_FONT_DEFAULT,kMuted);
    calChong=label(calDetail,12,68,216,LV_FONT_DEFAULT,kMuted);
    for(auto [y,color,part]:{std::tuple{94,0x22c55e,&calYi},std::tuple{176,0xe5484d,&calJi}}){
        auto* card=lv_obj_create(calDetail);box(card,8,y,224,78,kPanel,10);
        auto* badge=lv_obj_create(card);box(badge,8,8,26,26,color,13);
        auto* mark=label(badge,0,3,26,LV_FONT_DEFAULT,0xffffff);setTextAlign(mark,LV_TEXT_ALIGN_CENTER,0);setLabel(mark,part==&calYi?"宜":"忌");
        *part=label(card,40,4,178,LV_FONT_DEFAULT,kText);lv_label_set_long_mode(*part,LV_LABEL_LONG_DOT);lv_obj_set_height(*part,70);
    }
    calMoon=lv_obj_create(calendarRoot);box(calMoon,8,2,224,255,kBg);lv_obj_add_event_cb(calMoon,drawCalMoon,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderAlmanac(const Model& m){
    const Date date{m.calendarYear,m.calendarMonth,m.calendarDay};char text[96];
    static const char* const week[]{"日","一","二","三","四","五","六"};
    snprintf(text,sizeof(text),"%d年%d月%d日 星期%s",date.year,date.month,date.day,week[weekday(date)]);setLabel(calDate,text);
    LunarDate lunar{};toLunar(date,lunar);const char* name=festival(date,lunar);const int off=holiday(date);
    snprintf(text,sizeof(text),"农历%s%s%s%s%s%s",lunar.leap?"闰":"",lunarMonthName(lunar.month),lunarDayName(lunar.day),name?" · ":"",name?name:"",off==1?" · 休":off==2?" · 补班":"");setLabel(calLunar,text);
    Almanac a{};if(!almanac(date,a)){setLabel(calGanzhi,"");setLabel(calChong,"");setLabel(calYi,"");setLabel(calJi,"");return;}
    char y[7],mo[7],d[7],c[7];ganzhiName(a.year,y);ganzhiName(a.month,mo);ganzhiName(a.day,d);ganzhiName(a.chong,c);
    snprintf(text,sizeof(text),"%s年 %s月 %s日 · 属%s",y,mo,d,zodiac(lunar.year));setLabel(calGanzhi,text);
    snprintf(text,sizeof(text),"冲%s(%s) · 煞%s",zodiacOfBranch(a.chong%12),c,a.sha);setLabel(calChong,text);
    static char words[2][256];
    for(int part=0;part<2;++part){
        const uint8_t* list=part?a.ji:a.yi;const int count=part?a.jiCount:a.yiCount;words[part][0]=0;
        for(int i=0;i<count;++i){if(i)strcat(words[part]," ");strcat(words[part],almanacWord(list[i]));}
        setLabel(part?calJi:calYi,count?words[part]:"无");
    }
}
void renderCalendar(const Model& m){
    drawModel=&m;char text[48];
    const bool month=m.calendarView==0;
    visible(calDetail,m.calendarView==1);visible(calMoon,m.calendarView==2);visible(calendarGrid,month);visible(calendarTitle,month);visible(calendarYearName,month);
    if(m.calendarView==1){renderAlmanac(m);return;}
    if(m.calendarView==2){drawModel=&m;lv_obj_invalidate(calMoon);return;}
    snprintf(text,sizeof(text),"%d年%d月",m.calendarYear,m.calendarMonth);setLabel(calendarTitle,text);
    LunarDate lunar{};toLunar({m.calendarYear,m.calendarMonth,15},lunar);char stem[7];ganzhi(lunar.year,stem);
    snprintf(text,sizeof(text),"%s%s年%s",stem,zodiac(lunar.year),holidayYearKnown(m.calendarYear)?"":" · 无假期数据");setLabel(calendarYearName,text);
    // Redraw the month only when it, or today, changes.
    static int shown=-1;const time_t now=time(nullptr);const int key=((m.calendarYear*12+m.calendarMonth)*32+m.calendarDay)*40+static_cast<int>(now/86400%40);
    if(key!=shown){shown=key;lv_obj_invalidate(calendarGrid);}
}
// 倒数日: one event as a big number, or the whole list.
void initCountdown(lv_obj_t* screen){
    countRoot=lv_obj_create(screen);box(countRoot,0,35,240,257,kBg);
    auto* card=lv_obj_create(countRoot);box(card,8,6,224,214,kPanel,16);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    countName=label(card,8,14,208,LV_FONT_DEFAULT,kText);setTextAlign(countName,LV_TEXT_ALIGN_CENTER,0);
    countDays=label(card,0,52,224,&lv_font_montserrat_48,kAccent);setTextAlign(countDays,LV_TEXT_ALIGN_CENTER,0);
    countUnit=label(card,8,112,208,LV_FONT_DEFAULT,kMuted);setTextAlign(countUnit,LV_TEXT_ALIGN_CENTER,0);
    countDate=label(card,8,150,208,LV_FONT_DEFAULT,kMuted);setTextAlign(countDate,LV_TEXT_ALIGN_CENTER,0);
    countList=label(countRoot,16,12,208,LV_FONT_DEFAULT,kText);lv_label_set_long_mode(countList,LV_LABEL_LONG_CLIP);lv_obj_set_height(countList,206);
    lv_label_set_recolor(countList,true);
    countInfo=label(countRoot,10,226,220,LV_FONT_DEFAULT,kMuted);setTextAlign(countInfo,LV_TEXT_ALIGN_CENTER,0);
    static const char* const notes[]{"例：妈妈生日 农历八月十五","生日、纪念日每年重复"};
    countVoice=voiceLayer(countRoot,notes);
}
void renderCountdown(const Model& m){
    CountdownItem items[20];const time_t now=time(nullptr);tm t{};localtime_r(&now,&t);
    const unsigned count=m.clockValid?m.countdownItems({t.tm_year+1900,t.tm_mon+1,t.tm_mday},t.tm_hour*60+t.tm_min,items,std::size(items)):0;
    const unsigned index=count?m.countdownIndex%count:0;char text[160];
    visible(countVoice,m.voiceBusy());if(m.voiceBusy()){drawModel=&m;lv_obj_invalidate(countVoice);}
    visible(lv_obj_get_parent(countName),!m.countdownList);visible(countList,m.countdownList);
    if(!count){setLabel(countName,"等待时间同步");setLabel(countDays,"--");setLabel(countUnit,"");setLabel(countDate,"");setLabel(countInfo,"");return;}
    if(m.countdownList){
        // Seven rows around the focus; days aligned right by padding is unreliable in a proportional font, so "·".
        static char list[7*72];list[0]=0;const unsigned first=index<3||count<=7?0:std::min(index-3,count-7);
        for(unsigned i=first;i<count&&i<first+7;++i){
            const auto& it=items[i];char row[72];
            if(it.minutes>=0)snprintf(row,sizeof(row),"%s%s · %d:%02d#\n",i==index?"#4bffc4 > ":"#eaf4ff ",it.name,it.minutes/60,it.minutes%60);
            else snprintf(row,sizeof(row),"%s%s · %s%d 天#\n",i==index?"#4bffc4 > ":"#eaf4ff ",it.name,it.days<0?"已 ":"",std::abs(it.days));
            strcat(list,row);
        }
        setLabel(countList,list);
    }else{
        const auto& it=items[index];
        if(it.minutes>=0){
            snprintf(text,sizeof(text),"距离%s还有",it.name);setLabel(countName,text);
            snprintf(text,sizeof(text),"%d:%02d",it.minutes/60,it.minutes%60);setLabel(countDays,text);setLabel(countUnit,"小时 : 分钟");
            setLabel(countDate,"打工人，坚持住");
        }else{
            snprintf(text,sizeof(text),it.days<0?"%s 已经":it.days?"距离 %s 还有":"%s",it.name);setLabel(countName,text);
            snprintf(text,sizeof(text),"%d",std::abs(it.days));setLabel(countDays,it.days?text:"今天");
            setTextFont(countDays,it.days?&lv_font_montserrat_48:static_cast<const lv_font_t*>(&passport_status_font),0);
            setLabel(countUnit,it.days?"天":"就是今天");
            static const char* const week[]{"日","一","二","三","四","五","六"};LunarDate lunar{};toLunar(it.date,lunar);
            snprintf(text,sizeof(text),"%d-%02d-%02d 周%s · %s%s%s",it.date.year,it.date.month,it.date.day,week[weekday(it.date)],lunar.leap?"闰":"",lunarMonthName(lunar.month),lunarDayName(lunar.day));
            setLabel(countDate,text);
        }
        setTextColor(countDays,lv_color_hex(it.days<0?0xff8a73:kAccent),0);
    }
    // The line under the card: the delete question, what the last phrase or delete did, or the place in the list.
    if(m.countdownArmed)snprintf(text,sizeof(text),"再长按下键删除「%s」",items[index].name);else if(*m.lastNotice)snprintf(text,sizeof(text),"%s",m.lastNotice);else snprintf(text,sizeof(text),"%u / %u",index+1,count);
    setLabel(countInfo,text);setTextColor(countInfo,lv_color_hex(m.countdownArmed?0xff5a5a:*m.lastNotice?kWarn:kMuted),0);
}
// 财经新闻: index quotes, then one headline card at a time.
void initNews(lv_obj_t* screen){
    newsRoot=lv_obj_create(screen);box(newsRoot,0,35,240,257,kBg);
    newsQuotes=label(newsRoot,10,3,220,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(newsQuotes,20);
    lv_label_set_long_mode(newsQuotes,LV_LABEL_LONG_SCROLL_CIRCULAR);lv_obj_set_style_anim_duration(newsQuotes,lv_anim_speed_clamped(30,800,20000),0);
    auto* card=lv_obj_create(newsRoot);box(card,8,26,224,226,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    newsMeta=label(card,12,8,200,LV_FONT_DEFAULT,kAccent);
    newsTitle=label(card,12,32,200,LV_FONT_DEFAULT,kText);lv_label_set_long_mode(newsTitle,LV_LABEL_LONG_DOT);lv_obj_set_height(newsTitle,60);
    newsDigest=label(card,12,98,200,LV_FONT_DEFAULT,kMuted);lv_label_set_long_mode(newsDigest,LV_LABEL_LONG_CLIP);
    newsLines=label(card,12,200,200,LV_FONT_DEFAULT,0x5f7f94);lv_obj_set_style_text_align(newsLines,LV_TEXT_ALIGN_RIGHT,0);
}
void renderNews(const Model& m){
    setLabel(newsQuotes,m.liveLines[0]);
    if(!m.newsCount){setLabel(newsMeta,"");visible(newsTitle,true);setLabel(newsTitle,m.livePhase==LivePhase::Error?m.liveState:"正在获取快讯");setLabel(newsDigest,"");setLabel(newsLines,"");return;}
    const auto& item=m.page.news.items[m.newsIndex%m.newsCount];char text[48];
    if(m.newsWant!=m.newsPage)snprintf(text,sizeof(text),"正在加载%s的快讯",m.newsWant>m.newsPage?"更早":"较新");
    else if(m.newsPage>1)snprintf(text,sizeof(text),"%s  ·  第 %u 页  ·  %u / %u",item.time,m.newsPage,m.newsIndex%m.newsCount+1,m.newsCount);
    else snprintf(text,sizeof(text),"%s  ·  %u / %u",item.time,m.newsIndex%m.newsCount+1,m.newsCount);
    setLabel(newsMeta,text);
    setLabel(newsTitle,item.title);
    // The text from line newsScroll, broken here into the lines counted for the keys; past the first screen it
    // takes the headline's place.
    const auto& n=m.page.news;const bool more=m.newsScroll;visible(newsTitle,!more);
    lv_obj_set_y(newsDigest,more?32:98);lv_obj_set_height(newsDigest,(more?Model::kNewsMoreLines:Model::kNewsLines)*20);
    if(n.textFor!=m.newsIndex%m.newsCount){setLabel(newsDigest,"正在加载正文…");setLabel(newsLines,"");return;}
    char shown[640]{};const char* s=n.text;unsigned line=0,count=0;
    for(;*s;++line){
        const char* start=s;s=lineEnd(s,200);
        if(line<m.newsScroll||count==(more?Model::kNewsMoreLines:Model::kNewsLines))continue;
        const size_t used=strlen(shown);if(used+(s-start)+2>sizeof(shown))continue;
        snprintf(shown+used,sizeof(shown)-used,"%s%.*s",count++?"\n":"",int(s-start),start);
    }
    setLabel(newsDigest,shown);
    if(line>Model::kNewsLines){snprintf(text,sizeof(text),"%u-%u / %u 行",m.newsScroll+1,m.newsScroll+count,line);setLabel(newsLines,text);}else setLabel(newsLines,"");
}
// 蓝牙声纳: a disc where each device keeps its own bearing (from its address) and sits nearer the centre the stronger it is.
const char* companyName(uint16_t id){
    switch(id){case 0x004c:return "Apple";case 0x0006:return "微软";case 0x0075:return "三星";case 0x00e0:return "谷歌";case 0x027d:return "华为";
    case 0x038f:return "小米";case 0x0157:return "华米";case 0x02e5:return "乐鑫";case 0x0059:return "Nordic";case 0x0171:return "亚马逊";default:return nullptr;}
}
void sonarLabel(const Model::SonarDevice& d,char* out,size_t size){
    if(*d.name)snprintf(out,size,"%s",d.name);
    else if(companyName(d.company))snprintf(out,size,"%s 设备",companyName(d.company));
    else snprintf(out,size,"未知设备 %02X%02X",d.addr[1],d.addr[0]);
}
int sonarRadius(int rssi){return std::clamp(100-(rssi+100)*86/70,14,100);}  // stronger = nearer the centre
float sonarMeters(int rssi){return std::pow(10.0f,(-59.0f-rssi)/22.0f);}  // ponytail: one path-loss guess for every device
void drawSonar(lv_event_t* e){
    const auto& m=*drawModel;auto* layer=lv_event_get_layer(e);lv_area_t area;lv_obj_get_coords(sonarView,&area);
    uint8_t order[Model::kSonarSlots];const unsigned count=m.sonarVisible(order);
    lv_draw_label_dsc_t text;lv_draw_label_dsc_init(&text);text.text_local=1;text.font=LV_FONT_DEFAULT;
    if(m.sonarList){
        unsigned focus=0;for(unsigned i=0;i<count;++i)if(!memcmp(m.sonar[order[i]].addr,m.sonarFocus,6))focus=i;
        const unsigned first=focus<4||count<=9?0:std::min(focus-4,count-9);
        for(unsigned i=first;i<count&&i<first+9;++i){
            const auto& d=m.sonar[order[i]];const int y=area.y1+(i-first)*22;char name[40],level[12];sonarLabel(d,name,sizeof(name));
            if(i==focus){lv_draw_rect_dsc_t back;lv_draw_rect_dsc_init(&back);back.radius=6;back.bg_color=lv_color_hex(kPanel);lv_area_t row{area.x1,y,area.x2,y+21};lv_draw_rect(layer,&back,&row);}
            lv_draw_rect_dsc_t bar;lv_draw_rect_dsc_init(&bar);bar.radius=2;bar.bg_color=lv_color_hex(i==focus?kAccent:kBorder);
            lv_area_t fill{area.x2-56,y+8,area.x2-56+std::clamp((d.rssi+100)*50/70,2,50),y+13};lv_draw_rect(layer,&bar,&fill);
            text.align=LV_TEXT_ALIGN_LEFT;text.color=lv_color_hex(i==focus?kAccent:kText);text.text=name;lv_area_t left{area.x1+6,y+1,area.x2-100,y+21};lv_draw_label(layer,&text,&left);
            snprintf(level,sizeof(level),"%d",d.rssi);text.align=LV_TEXT_ALIGN_RIGHT;text.color=lv_color_hex(kMuted);text.text=level;lv_area_t right{area.x2-100,y+1,area.x2-62,y+21};lv_draw_label(layer,&text,&right);
        }
        return;
    }
    const int cx=(area.x1+area.x2)/2,cy=(area.y1+area.y2)/2;
    lv_draw_arc_dsc_t ring;lv_draw_arc_dsc_init(&ring);ring.center={cx,cy};ring.start_angle=0;ring.end_angle=360;ring.width=1;ring.color=lv_color_hex(kBorder);
    for(int r:{34,67,100}){ring.radius=r;lv_draw_arc(layer,&ring);}
    lv_draw_rect_dsc_t dot;lv_draw_rect_dsc_init(&dot);dot.radius=LV_RADIUS_CIRCLE;
    dot.bg_color=lv_color_hex(kAccent);lv_area_t centre{cx-3,cy-3,cx+3,cy+3};lv_draw_rect(layer,&dot,&centre);
    for(unsigned i=0;i<count;++i){
        const auto& d=m.sonar[order[i]];const bool focus=!memcmp(d.addr,m.sonarFocus,6);
        const int angle=(d.addr[0]*7+d.addr[1]*13+d.addr[2]*29)%360,r=sonarRadius(d.rssi),size=focus?6:3;
        const int x=cx+lv_trigo_sin(angle)*r/32767,y=cy-lv_trigo_cos(angle)*r/32767;
        dot.bg_color=lv_color_hex(focus?kAccent:companyName(d.company)?0x7fb6ff:0x9bbbd1);
        lv_area_t at{x-size,y-size,x+size,y+size};lv_draw_rect(layer,&dot,&at);
        if(focus){ring.color=lv_color_hex(kAccent);ring.width=2;ring.radius=10;ring.center={x,y};lv_draw_arc(layer,&ring);}
    }
    // The connected phone: a green square at the top, tracked by its link RSSI.
    if(m.sonarPeerRssi){
        const int r=sonarRadius(m.sonarPeerRssi);lv_draw_rect_dsc_t box;lv_draw_rect_dsc_init(&box);box.bg_opa=LV_OPA_TRANSP;
        box.border_color=lv_color_hex(0x22c55e);box.border_width=2;box.radius=2;lv_area_t at{cx-8,cy-r-8,cx+8,cy-r+8};lv_draw_rect(layer,&box,&at);
    }
}
void initSonar(lv_obj_t* screen){
    sonarRoot=lv_obj_create(screen);box(sonarRoot,0,35,240,257,kBg);
    sonarView=lv_obj_create(sonarRoot);box(sonarView,14,2,212,204,kBg);
    lv_obj_add_event_cb(sonarView,drawSonar,LV_EVENT_DRAW_MAIN,nullptr);
    sonarMode=label(sonarRoot,8,2,120,LV_FONT_DEFAULT,kMuted);
    sonarInfo=label(sonarRoot,10,210,220,LV_FONT_DEFAULT,kText);lv_obj_set_height(sonarInfo,lv_font_get_line_height(LV_FONT_DEFAULT));
    sonarDetail=label(sonarRoot,10,232,220,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(sonarDetail,lv_font_get_line_height(LV_FONT_DEFAULT));
}
void renderSonar(const Runtime& runtime){
    const auto& m=runtime.model;drawModel=&m;uint8_t order[Model::kSonarSlots];const unsigned count=m.sonarVisible(order);char text[96];
    snprintf(text,sizeof(text),"%s %u",m.sonarNamed?"有标识":"未标识",count);setLabel(sonarMode,text);visible(sonarMode,!m.sonarList);
    const Model::SonarDevice* focus=nullptr;for(unsigned i=0;i<count;++i)if(!memcmp(m.sonar[order[i]].addr,m.sonarFocus,6))focus=&m.sonar[order[i]];
    if(m.sonarPeerRssi&&!focus){snprintf(text,sizeof(text),"■ 已连接手机  %d dBm",m.sonarPeerRssi);setLabel(sonarInfo,text);setTextColor(sonarInfo,lv_color_hex(0x22c55e),0);
        snprintf(text,sizeof(text),"约 %.1f 米 · 上下键选择其他设备",sonarMeters(m.sonarPeerRssi));setLabel(sonarDetail,text);}
    else if(focus){char name[40];sonarLabel(*focus,name,sizeof(name));snprintf(text,sizeof(text),"%d dBm  %s",focus->rssi,name);setLabel(sonarInfo,text);setTextColor(sonarInfo,lv_color_hex(kAccent),0);
        snprintf(text,sizeof(text),"约 %.1f 米 · %02X:%02X:%02X",sonarMeters(focus->rssi),focus->addr[2],focus->addr[1],focus->addr[0]);setLabel(sonarDetail,text);}
    else{setTextColor(sonarInfo,lv_color_hex(kText),0);setLabel(sonarInfo,!runtime.settings().bluetoothEnabled?"蓝牙已关闭":count?"上下键选择设备":"正在扫描周围蓝牙设备");
        setLabel(sonarDetail,!runtime.settings().bluetoothEnabled?"请在系统设置中打开蓝牙":"越靠内圈信号越强");}
    lv_obj_invalidate(sonarView);
}
// 汇率: one row per currency, priced in the base currency (100 or 10000 units when one unit is worth little).
void initRate(lv_obj_t* screen){
    rateRoot=lv_obj_create(screen);box(rateRoot,0,35,240,257,kBg);
    rateHead=label(rateRoot,12,4,216,LV_FONT_DEFAULT,kMuted);lv_label_set_long_mode(rateHead,LV_LABEL_LONG_MODE_DOTS);lv_obj_set_height(rateHead,20);
    for(unsigned i=0;i<RateBoard::kTargets;++i){
        rateRows[i]=lv_obj_create(rateRoot);box(rateRows[i],8,28+i*34,224,30,kPanel,8);
        rateNames[i]=label(rateRows[i],10,5,110,LV_FONT_DEFAULT,kText);
        rateValues[i]=label(rateRows[i],110,5,104,LV_FONT_DEFAULT,kAccent);setTextAlign(rateValues[i],LV_TEXT_ALIGN_RIGHT,0);
    }
    rateFoot=label(rateRoot,12,234,216,LV_FONT_DEFAULT,kMuted);
    static const char* const notes[]{"例：添加日元、泰铢","计价货币不变"};
    rateVoice=voiceLayer(rateRoot,notes);
}
void renderRate(const Model& m){
    const time_t at=m.rateUpdated;tm t{};localtime_r(&at,&t);
    visible(rateVoice,m.voiceBusy());if(m.voiceBusy()){drawModel=&m;lv_obj_invalidate(rateVoice);}
    char text[96];snprintf(text,sizeof(text),"以%s计价 · %d-%02d 更新",currencyName(m.rateBase),t.tm_mon+1,t.tm_mday);
    // The top line: the delete question, what the last phrase or delete did, or the base and the date.
    if(m.rateArmed&&m.rateCount)snprintf(text,sizeof(text),"再长按下键删除%s",currencyName(m.rates[m.rateIndex%m.rateCount].code));
    setLabel(rateHead,m.rateArmed||(m.rateCount&&!*m.lastNotice)?text:*m.lastNotice?m.lastNotice:m.livePhase==LivePhase::Error?m.liveState:"正在获取汇率");
    setTextColor(rateHead,lv_color_hex(m.rateArmed?0xff5a5a:*m.lastNotice?kWarn:kMuted),0);
    for(unsigned i=0;i<RateBoard::kTargets;++i){
        visible(rateRows[i],i<m.rateCount);if(i>=m.rateCount)continue;
        const auto& r=m.rates[i];const double each=1/r.value;int units=1;while(each*units<1&&units<10000)units*=100;
        const bool on=i==m.rateIndex;
        if(units==1)snprintf(text,sizeof(text),"%s %s",currencyName(r.code),r.code);else snprintf(text,sizeof(text),"%d %s",units,currencyName(r.code));
        setLabel(rateNames[i],text);snprintf(text,sizeof(text),each*units>=100?"%.2f":"%.4f",each*units);setLabel(rateValues[i],text);
        setBgColor(rateRows[i],lv_color_hex(on?kAccent:kPanel),0);setTextColor(rateNames[i],lv_color_hex(on?kBg:kText),0);setTextColor(rateValues[i],lv_color_hex(on?kBg:kAccent),0);
    }
    if(!m.rateCount){setLabel(rateFoot,"");return;}
    const auto& r=m.rates[m.rateIndex%m.rateCount];
    snprintf(text,sizeof(text),"1 %s = %.4g %s",currencyName(m.rateBase),r.value,currencyName(r.code));setLabel(rateFoot,text);
    // The picked row turns red while its delete is asked.
    if(m.rateArmed)setBgColor(rateRows[m.rateIndex%m.rateCount],lv_color_hex(0xff5a5a),0);
}
// 历史今日: one event card (year, kind, title) with the next few listed under it.
void initHistory(lv_obj_t* screen){
    histRoot=lv_obj_create(screen);box(histRoot,0,35,240,257,kBg);
    histMeta=label(histRoot,12,4,216,LV_FONT_DEFAULT,kMuted);
    auto* card=lv_obj_create(histRoot);box(card,8,28,224,128,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    histYear=label(card,12,8,120,&lv_font_montserrat_24,kAccent);
    histKind=label(card,130,12,80,LV_FONT_DEFAULT,kMuted);setTextAlign(histKind,LV_TEXT_ALIGN_RIGHT,0);
    histTitle=label(card,12,44,200,LV_FONT_DEFAULT,kText);lv_obj_set_height(histTitle,72);
    for(unsigned i=0;i<3;++i){histNext[i]=label(histRoot,12,166+i*28,216,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(histNext[i],lv_font_get_line_height(LV_FONT_DEFAULT));}
}
void renderHistory(const Model& m){
    char text[96];
    if(!m.eventCount){setLabel(histMeta,"");setLabel(histYear,"");setLabel(histKind,"");setLabel(histTitle,m.livePhase==LivePhase::Error?m.liveState:"正在获取历史今日");for(auto* l:histNext)setLabel(l,"");return;}
    const time_t now=time(nullptr);tm t{};localtime_r(&now,&t);
    if(m.clockValid)snprintf(text,sizeof(text),"%d月%d日  ·  %u / %u",t.tm_mon+1,t.tm_mday,m.eventIndex+1,m.eventCount);else snprintf(text,sizeof(text),"%u / %u",m.eventIndex+1,m.eventCount);
    setLabel(histMeta,text);
    static const char* const kinds[]{"大事","出生","逝世"};static const uint32_t colors[]{kAccent,0x5fd3ff,0x9bbbd1};
    const auto& e=m.page.events[m.eventIndex%m.eventCount];
    snprintf(text,sizeof(text),"%d",e.year);setLabel(histYear,text);setTextColor(histYear,lv_color_hex(colors[e.type%3]),0);
    snprintf(text,sizeof(text),"%s%s",e.year<0?"公元前 · ":"",kinds[e.type%3]);setLabel(histKind,text);setLabel(histTitle,e.title);
    for(unsigned i=0;i<3;++i){
        const unsigned at=m.eventIndex+1+i;
        if(at<m.eventCount){snprintf(text,sizeof(text),"%d  %s",m.page.events[at].year,m.page.events[at].title);setLabel(histNext[i],text);}else setLabel(histNext[i],"");
    }
}
// 每日一句: the English sentence large, the translation under it.
void initDaily(lv_obj_t* screen){
    dailyRoot=lv_obj_create(screen);box(dailyRoot,0,35,240,257,kBg);
    dailyMeta=label(dailyRoot,12,4,216,LV_FONT_DEFAULT,kMuted);
    auto* card=lv_obj_create(dailyRoot);box(card,8,28,224,224,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    dailyEnText=label(card,12,12,200,&lv_font_montserrat_20,kText);lv_obj_set_height(dailyEnText,120);
    dailyCnText=label(card,12,140,200,LV_FONT_DEFAULT,kAccent);lv_obj_set_height(dailyCnText,76);
}
void renderDaily(const Model& m){
    char text[64];
    if(m.dailyWant!=m.dailyBack)snprintf(text,sizeof(text),"正在加载%s一天",m.dailyWant>m.dailyBack?"前":"后");
    else if(!*m.page.daily.en)snprintf(text,sizeof(text),"%s",m.livePhase==LivePhase::Error?m.liveState:"正在获取每日一句");
    else if(m.dailyBack)snprintf(text,sizeof(text),"%s  ·  %u 天前",m.page.daily.date,m.dailyBack);
    else snprintf(text,sizeof(text),"%s  ·  今天",m.page.daily.date);
    setLabel(dailyMeta,text);setLabel(dailyEnText,m.page.daily.en);setLabel(dailyCnText,m.page.daily.cn);
}
// "3 分钟前" from an epoch; empty without a valid clock.
void ago(uint32_t at,char* out,size_t size){
    const long long now=time(nullptr),d=now-at;
    if(now<1700000000||!at)*out=0;else if(d<60)snprintf(out,size,"刚刚");else if(d<3600)snprintf(out,size,"%lld 分钟前",d/60);
    else if(d<86400)snprintf(out,size,"%lld 小时前",d/3600);else snprintf(out,size,"%lld 天前",d/86400);
}
lv_obj_t* card(lv_obj_t* parent,int y,int h){auto* c=lv_obj_create(parent);box(c,8,y,224,h,kPanel,12);lv_obj_set_style_border_width(c,1,0);lv_obj_set_style_border_color(c,lv_color_hex(kBorder),0);return c;}
lv_obj_t* textRow(lv_obj_t* parent,int x,int y,int w,uint32_t color){auto* l=label(parent,x,y,w,LV_FONT_DEFAULT,color);lv_obj_set_height(l,lv_font_get_line_height(LV_FONT_DEFAULT));return l;}
// 地震速报: the chosen quake large (magnitude coloured by strength), the next few listed under it.
lv_obj_t *quakeRoot,*quakeMeta,*quakeMag,*quakeLevel,*quakePlace,*quakeDepth,*quakeTime,*quakeNext[4];
uint32_t quakeColor(unsigned mag){return mag>=60?0xff5a5a:mag>=45?kWarn:mag>=30?kAccent:kMuted;}
void initQuake(lv_obj_t* screen){
    quakeRoot=lv_obj_create(screen);box(quakeRoot,0,35,240,257,kBg);
    quakeMeta=label(quakeRoot,12,4,216,LV_FONT_DEFAULT,kMuted);
    auto* c=card(quakeRoot,28,124);
    quakeMag=label(c,4,8,100,&lv_font_montserrat_48,kAccent);setTextAlign(quakeMag,LV_TEXT_ALIGN_CENTER,0);
    quakeLevel=textRow(c,4,64,100,kMuted);setTextAlign(quakeLevel,LV_TEXT_ALIGN_CENTER,0);
    quakePlace=label(c,110,10,102,LV_FONT_DEFAULT,kText);lv_obj_set_height(quakePlace,44);quakeDepth=textRow(c,110,60,102,kMuted);
    quakeTime=textRow(c,12,94,200,kMuted);
    for(unsigned i=0;i<4;++i)quakeNext[i]=textRow(quakeRoot,12,162+i*24,216,kMuted);
}
void renderQuake(const Model& m){
    char text[112];
    if(!m.quakeCount){
        for(auto* l:{quakeMeta,quakeMag,quakeLevel,quakeDepth,quakeTime,quakeNext[0],quakeNext[1],quakeNext[2],quakeNext[3]})setLabel(l,"");
        setLabel(quakePlace,m.livePhase==LivePhase::Error?m.liveState:"正在获取地震速报");return;
    }
    const auto& q=m.page.quakes[m.quakeIndex%m.quakeCount];
    if(m.livePhase==LivePhase::Ready)snprintf(text,sizeof(text),"中国地震台网  ·  %u / %u",m.quakeIndex%m.quakeCount+1,m.quakeCount);
    else snprintf(text,sizeof(text),"%s",m.livePhase==LivePhase::Error?m.liveState:"正在刷新");
    setLabel(quakeMeta,text);
    snprintf(text,sizeof(text),"%u.%u",q.mag/10,q.mag%10);setLabel(quakeMag,text);setTextColor(quakeMag,lv_color_hex(quakeColor(q.mag)),0);
    setLabel(quakeLevel,q.mag>=70?"大地震":q.mag>=60?"强震":q.mag>=45?"中强震":q.mag>=30?"有感地震":"微震");
    setLabel(quakePlace,q.place);
    snprintf(text,sizeof(text),"深 %u km",q.depth);setLabel(quakeDepth,text);
    char when[24],rel[24];time_t at=q.at;tm t{};localtime_r(&at,&t);strftime(when,sizeof(when),"%m月%d日 %H:%M",&t);ago(q.at,rel,sizeof(rel));
    snprintf(text,sizeof(text),"%s%s%s",when,*rel?"  ·  ":"",rel);setLabel(quakeTime,text);
    for(unsigned i=0;i<4;++i){
        const unsigned index=m.quakeIndex%m.quakeCount+1+i;
        if(index>=m.quakeCount){setLabel(quakeNext[i],"");continue;}
        const auto& n=m.page.quakes[index];at=n.at;localtime_r(&at,&t);strftime(when,sizeof(when),"%H:%M",&t);
        snprintf(text,sizeof(text),"%s  M%u.%u  %s",when,n.mag/10,n.mag%10,n.place);setLabel(quakeNext[i],text);
        setTextColor(quakeNext[i],lv_color_hex(n.mag>=45?quakeColor(n.mag):kMuted),0);
    }
}
// 知识问答: the question on a card, four answer rows; once answered the right one turns green, a wrong pick red.
lv_obj_t *quizRoot,*quizMeta,*quizScore,*quizText,*quizRows[4],*quizLabels[4];
void initQuiz(lv_obj_t* screen){
    quizRoot=lv_obj_create(screen);box(quizRoot,0,35,240,257,kBg);
    quizMeta=label(quizRoot,12,4,150,LV_FONT_DEFAULT,kMuted);
    quizScore=label(quizRoot,150,4,78,LV_FONT_DEFAULT,kAccent);setTextAlign(quizScore,LV_TEXT_ALIGN_RIGHT,0);
    auto* c=card(quizRoot,28,104);quizText=label(c,12,8,200,LV_FONT_DEFAULT,kText);lv_obj_set_height(quizText,88);
    for(unsigned i=0;i<4;++i){quizRows[i]=lv_obj_create(quizRoot);box(quizRows[i],8,138+i*30,224,27,kPanel,8);quizLabels[i]=textRow(quizRows[i],10,3,204,kText);}
}
void renderQuiz(const Model& m){
    char text[112];
    snprintf(text,sizeof(text),"答对 %u/%u",m.quizRight,m.quizAsked);setLabel(quizScore,m.quizAsked?text:"");
    for(unsigned i=0;i<4;++i)visible(quizRows[i],i<m.quizCount);
    if(!m.quizCount){setLabel(quizMeta,"");setLabel(quizText,m.livePhase==LivePhase::Error?m.liveState:"正在获取题目");return;}
    const char* level=!strcmp(m.page.quiz.level,"easy")?"简单":!strcmp(m.page.quiz.level,"hard")?"困难":"中等";
    if(m.quizWant)snprintf(text,sizeof(text),"%s",m.livePhase==LivePhase::Error?m.liveState:"正在加载下一题");else snprintf(text,sizeof(text),"%s · %s",level,m.page.quiz.category);
    setLabel(quizMeta,text);setLabel(quizText,m.page.quiz.question);
    for(unsigned i=0;i<m.quizCount;++i){
        const bool right=m.quizShown&&i==m.quizCorrect,wrong=m.quizShown&&i==m.quizPick&&!right,picked=!m.quizShown&&i==m.quizPick;
        setBgColor(quizRows[i],lv_color_hex(right?0x22c55e:wrong?0xe5484d:picked?kAccent:kPanel),0);
        setTextColor(quizLabels[i],lv_color_hex(right||wrong||picked?kBg:m.quizShown?kMuted:kText),0);
        snprintf(text,sizeof(text),"%c  %s",'A'+i,m.page.quiz.answers[i]);setLabel(quizLabels[i],text);
    }
}
// Hacker News: the chosen story on a card (title, site, points, comments, age), the next titles under it.
lv_obj_t *hnRoot,*hnMeta,*hnTitle,*hnSite,*hnStats,*hnNext[3];
void initHacker(lv_obj_t* screen){
    hnRoot=lv_obj_create(screen);box(hnRoot,0,35,240,257,kBg);
    hnMeta=label(hnRoot,12,4,216,LV_FONT_DEFAULT,kMuted);
    auto* c=card(hnRoot,28,148);hnTitle=label(c,12,8,200,LV_FONT_DEFAULT,kText);lv_obj_set_height(hnTitle,84);
    hnSite=textRow(c,12,96,200,0xff8a3d);hnStats=textRow(c,12,120,200,kMuted);
    for(unsigned i=0;i<3;++i)hnNext[i]=textRow(hnRoot,12,186+i*24,216,kMuted);
}
void renderHacker(const Model& m){
    char text[128];
    if(!m.storyCount){for(auto* l:{hnMeta,hnSite,hnStats,hnNext[0],hnNext[1],hnNext[2]})setLabel(l,"");setLabel(hnTitle,m.livePhase==LivePhase::Error?m.liveState:"正在获取 Hacker News");return;}
    const unsigned at=m.storyIndex%m.storyCount;const auto& s=m.page.stories[at];
    if(m.livePhase==LivePhase::Ready)snprintf(text,sizeof(text),"Hacker News 首页  ·  %u / %u",at+1,m.storyCount);
    else snprintf(text,sizeof(text),"%s",m.livePhase==LivePhase::Error?m.liveState:"正在刷新");
    setLabel(hnMeta,text);
    setLabel(hnTitle,s.title);setLabel(hnSite,*s.site?s.site:"news.ycombinator.com");
    char rel[24];ago(s.at,rel,sizeof(rel));snprintf(text,sizeof(text),"%u 分 · %u 评论%s%s",s.points,s.comments,*rel?" · ":"",rel);setLabel(hnStats,text);
    for(unsigned i=0;i<3;++i){const unsigned n=at+1+i;if(n<m.storyCount){snprintf(text,sizeof(text),"%u  %s",m.page.stories[n].points,m.page.stories[n].title);setLabel(hnNext[i],text);}else setLabel(hnNext[i],"");}
}
// A row of tabs (WiFi 工具箱, 声音); long Up/Down moves between them.
void initTabs(lv_obj_t* parent,lv_obj_t** tabs,lv_obj_t** texts,std::initializer_list<const char*> names){
    const int width=216/static_cast<int>(names.size());auto* track=lv_obj_create(parent);box(track,12,4,width*names.size(),24,kPanel,12);unsigned i=0;
    for(auto* name:names){tabs[i]=lv_obj_create(track);box(tabs[i],i*width,0,width,24,kPanel,12);texts[i]=label(tabs[i],0,2,width,LV_FONT_DEFAULT,kMuted);setTextAlign(texts[i],LV_TEXT_ALIGN_CENTER,0);setLabel(texts[i],name);++i;}
}
void renderTabs(lv_obj_t** tabs,lv_obj_t** texts,unsigned count,unsigned on){
    for(unsigned i=0;i<count;++i){setBgColor(tabs[i],lv_color_hex(i==on?kAccent:kPanel),0);setTextColor(texts[i],lv_color_hex(i==on?kBg:kMuted),0);}
}
struct Painter{
    lv_layer_t* layer;lv_area_t area;
    void text(const char* s,int x1,int y,int x2,uint32_t color,lv_text_align_t align=LV_TEXT_ALIGN_LEFT,const lv_font_t* font=LV_FONT_DEFAULT){
        lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);d.text=s;d.text_local=1;d.font=font;d.color=lv_color_hex(color);d.align=align;
        lv_area_t a{area.x1+x1,area.y1+y,area.x1+x2,area.y1+y+lv_font_get_line_height(font)};lv_draw_label(layer,&d,&a);
    }
    void rect(int x1,int y1,int x2,int y2,uint32_t color,int radius=0){
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.radius=radius;d.bg_color=lv_color_hex(color);
        lv_area_t a{area.x1+x1,area.y1+y1,area.x1+x2,area.y1+y2};lv_draw_rect(layer,&d,&a);
    }
    void line(int x1,int y1,int x2,int y2,uint32_t color,int width=2){
        lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.width=width;d.color=lv_color_hex(color);d.round_start=d.round_end=1;
        d.p1={static_cast<lv_value_precise_t>(area.x1+x1),static_cast<lv_value_precise_t>(area.y1+y1)};d.p2={static_cast<lv_value_precise_t>(area.x1+x2),static_cast<lv_value_precise_t>(area.y1+y2)};lv_draw_line(layer,&d);
    }
};
Painter painter(lv_event_t* e){Painter p{lv_event_get_layer(e),{}};lv_obj_get_coords(lv_event_get_target_obj(e),&p.area);return p;}
// WiFi 工具箱: four tabs over one custom-drawn panel (a channel chart, ping rows, a device list or the wake target).
lv_obj_t *wifiRoot,*wifiTabs[4],*wifiTabText[4],*wifiView,*wifiInfo,*wifiStatus;
// Congestion per 2.4 GHz channel: each network weighs on its own channel and, less, on the two either side.
void wifiCongestion(const Model& m,int (&score)[14]){
    for(auto& s:score)s=0;
    for(unsigned i=0;i<m.wifiApCount;++i){const auto& ap=m.page.wifi.aps[i];for(int c=ap.channel-2;c<=ap.channel+2;++c)if(c>=1&&c<=13)score[c]+=std::max(0,ap.rssi+100)*(3-std::abs(c-ap.channel));}
}
void drawWifi(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);char text[48];
    if(m.wifiTab==0){
        int score[14];wifiCongestion(m,score);const int top=*std::max_element(score+1,score+14);
        const unsigned chosen=m.wifiApCount?m.page.wifi.aps[m.wifiIndex%m.wifiApCount].channel:0;
        for(int c=1;c<=13;++c){
            const int x=4+(c-1)*17,h=top?score[c]*120/top:0;
            p.rect(x,128-std::max(h,2),x+13,128,static_cast<unsigned>(c)==chosen?kAccent:c==1||c==6||c==11?0x2f7fa0:kBorder,3);
            if(c==1||c==6||c==11||c==13){snprintf(text,sizeof(text),"%d",c);p.text(text,x-6,132,x+19,kMuted,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);}
        }
        return;
    }
    if(m.wifiTab==1){
        for(unsigned i=0;i<m.pingCount;++i){
            const auto& r=m.page.wifi.pings[i];const int y=i*56;p.rect(0,y,223,y+50,kPanel,10);
            p.text(r.name,10,y+5,120,kText);p.text(r.host,10,y+27,140,kMuted);
            if(!r.sent){p.text(m.wifiBusy?"…":"--",120,y+12,214,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_24);continue;}
            if(r.lost==r.sent)snprintf(text,sizeof(text),"超时");else snprintf(text,sizeof(text),"%u ms",r.average);
            p.text(text,110,y+2,214,r.lost==r.sent?0xff5a5a:r.average<30?kAccent:r.average<100?kWarn:0xff5a5a,LV_TEXT_ALIGN_RIGHT,r.lost==r.sent?LV_FONT_DEFAULT:&lv_font_montserrat_24);
            if(r.lost<r.sent)snprintf(text,sizeof(text),"%u-%u ms 丢%u/%u",r.low,r.high,r.lost,r.sent);else snprintf(text,sizeof(text),"丢 %u/%u",r.lost,r.sent);
            p.text(text,100,y+29,214,kMuted,LV_TEXT_ALIGN_RIGHT);
        }
        return;
    }
    if(m.wifiTab==2){
        const unsigned focus=m.lanCount?m.wifiIndex%m.lanCount:0,first=focus<3||m.lanCount<=7?0:std::min<unsigned>(focus-3,m.lanCount-7);
        for(unsigned i=first;i<m.lanCount&&i<first+7;++i){
            const auto& d=m.page.wifi.lan[i];const int y=(i-first)*24;
            if(i==focus)p.rect(0,y,223,y+22,kPanel,6);
            snprintf(text,sizeof(text),"%u.%u.%u.%u",d.ip[0],d.ip[1],d.ip[2],d.ip[3]);p.text(text,8,y+1,140,i==focus?kAccent:kText);
            if(i==m.lanGateway)snprintf(text,sizeof(text),"网关");else if(i==m.lanSelf)snprintf(text,sizeof(text),"本机");
            else if(d.mac[0]&2)snprintf(text,sizeof(text),"随机地址");else snprintf(text,sizeof(text),"%02X:%02X:%02X",d.mac[3],d.mac[4],d.mac[5]);
            p.text(text,120,y+1,214,kMuted,LV_TEXT_ALIGN_RIGHT);
        }
        return;
    }
    p.text("唤醒目标",0,8,223,kMuted,LV_TEXT_ALIGN_CENTER);
    const auto* mac=m.tools.wakeMac;
    if(m.tools.wakeSet)snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);else snprintf(text,sizeof(text),"未设置");
    p.text(text,0,36,223,m.tools.wakeSet?kText:kWarn,LV_TEXT_ALIGN_CENTER,m.tools.wakeSet?&lv_font_montserrat_20:LV_FONT_DEFAULT);
    p.text("电脑需开启网络唤醒 (WOL)",0,84,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("并用有线网或支持 WOL 的网卡",0,106,223,kMuted,LV_TEXT_ALIGN_CENTER);
}
void initWifi(lv_obj_t* screen){
    wifiRoot=lv_obj_create(screen);box(wifiRoot,0,35,240,257,kBg);
    initTabs(wifiRoot,wifiTabs,wifiTabText,{"信道","延迟","设备","唤醒"});
    wifiView=lv_obj_create(wifiRoot);box(wifiView,8,36,224,168,kBg);lv_obj_add_event_cb(wifiView,drawWifi,LV_EVENT_DRAW_MAIN,nullptr);
    wifiInfo=textRow(wifiRoot,12,208,216,kText);wifiStatus=textRow(wifiRoot,12,232,216,kMuted);
}
void renderWifi(const Model& m){
    drawModel=&m;renderTabs(wifiTabs,wifiTabText,4,m.wifiTab);char info[112]{},status[112]{};
    if(m.wifiTab==0&&m.wifiApCount){
        const auto& ap=m.page.wifi.aps[m.wifiIndex%m.wifiApCount];int score[14];wifiCongestion(m,score);
        const int best=score[1]<=score[6]&&score[1]<=score[11]?1:score[6]<=score[11]?6:11;
        snprintf(info,sizeof(info),"%s  %d dBm%s",*ap.ssid?ap.ssid:"(隐藏网络)",ap.rssi,ap.open?" · 无密码":"");
        snprintf(status,sizeof(status),"信道 %u · 推荐 %d · 共 %u 个",ap.channel,best,m.wifiApCount);
    }else if(m.wifiTab==1&&m.dnsMs)snprintf(info,sizeof(info),"DNS 解析 %u ms",m.dnsMs);
    else if(m.wifiTab==2&&m.lanCount){
        const auto* mac=m.page.wifi.lan[m.wifiIndex%m.lanCount].mac;snprintf(info,sizeof(info),"MAC %02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        snprintf(status,sizeof(status),"局域网内 %u 台设备在线",m.lanCount);
    }else if(m.wifiTab==3&&m.wakeSentAt){const time_t at=m.wakeSentAt;tm t{};localtime_r(&at,&t);strftime(info,sizeof(info),"%H:%M:%S 已发送唤醒包",&t);}
    setLabel(wifiInfo,info);setLabel(wifiStatus,*m.wifiNote?m.wifiNote:status);
    lv_obj_invalidate(wifiView);
}
// 声音: three views under tabs; a big reading, then bars, a tuning meter or the beat dots drawn on one object.
lv_obj_t *soundRoot,*soundTabs[3],*soundTabText[3],*soundBig,*soundSub,*soundView,*soundInfo;
// Note, octave and cents of a frequency against the chosen A4.
void noteOf(float hz,unsigned a4,char* name,size_t size,int& cents){
    static const char* const names[]{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const float midi=69+12*std::log2(hz/a4);const int nearest=std::lround(midi);
    cents=std::lround((midi-nearest)*100);snprintf(name,size,"%s%d",names[(nearest%12+12)%12],nearest/12-1);
}
void drawSound(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);
    if(m.soundView==0){
        for(unsigned i=0;i<m.soundBars.size();++i){const int x=i*9+4,h=std::max(2,m.soundBars[i]*96/100);p.rect(x,100-h,x+6,100,i<8?kAccent:i<16?0x5fd3ff:0xb59cff,2);}
        p.text("80",0,104,40,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);p.text("1k",80,104,140,kMuted,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);p.text("8k Hz",160,104,223,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    }else if(m.soundView==1){
        p.rect(12,46,211,50,kBorder,2);
        for(int c=-50;c<=50;c+=25){const int x=112+c*2;p.rect(x-1,c?38:32,x+1,58,c?kBorder:kMuted);}
        p.text("-50",0,64,40,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);p.text("+50",183,64,223,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
        if(m.soundHz){char name[16];int cents;noteOf(m.soundHz/10.0f,m.tools.a4,name,sizeof(name),cents);const int x=112+std::clamp(cents,-50,50)*2;p.rect(x-3,26,x+3,70,std::abs(cents)<=5?0x22c55e:kWarn,3);}
    }else{
        for(unsigned i=0;i<4;++i){const int x=24+i*50;const bool on=m.soundRunning&&i==m.soundBeat;p.rect(x,30,x+26,56,on?(i?kAccent:kWarn):kBorder,13);}
    }
}
void initSound(lv_obj_t* screen){
    soundRoot=lv_obj_create(screen);box(soundRoot,0,35,240,257,kBg);
    initTabs(soundRoot,soundTabs,soundTabText,{"频谱","调音","节拍"});
    soundBig=label(soundRoot,0,34,240,&lv_font_montserrat_48,kText);setTextAlign(soundBig,LV_TEXT_ALIGN_CENTER,0);
    soundSub=textRow(soundRoot,0,90,240,kMuted);setTextAlign(soundSub,LV_TEXT_ALIGN_CENTER,0);
    soundView=lv_obj_create(soundRoot);box(soundView,8,112,224,120,kBg);lv_obj_add_event_cb(soundView,drawSound,LV_EVENT_DRAW_MAIN,nullptr);
    soundInfo=textRow(soundRoot,0,234,240,kMuted);setTextAlign(soundInfo,LV_TEXT_ALIGN_CENTER,0);
}
void renderSound(const Model& m){
    drawModel=&m;renderTabs(soundTabs,soundTabText,3,m.soundView);char text[64];const auto& t=m.tools;
    setTextColor(soundBig,lv_color_hex(kText),0);
    if(m.soundFailed&&m.soundView!=2){setLabel(soundBig,"--");setLabel(soundSub,"麦克风无法打开");setLabel(soundInfo,"");}
    else if(m.soundView==0){
        snprintf(text,sizeof(text),"%d",(m.soundDb+5)/10);setLabel(soundBig,m.soundDb?text:"--");
        snprintf(text,sizeof(text),"dB  ·  最大 %d",(m.soundMax+5)/10);setLabel(soundSub,m.soundMax?text:"dB");
        snprintf(text,sizeof(text),"估算值 · 校准 %+d dB",t.soundCal);setLabel(soundInfo,text);
    }else if(m.soundView==1){
        char name[16];int cents=0;
        if(m.soundHz){noteOf(m.soundHz/10.0f,t.a4,name,sizeof(name),cents);setLabel(soundBig,name);setTextColor(soundBig,lv_color_hex(std::abs(cents)<=5?0x22c55e:kText),0);
            snprintf(text,sizeof(text),"%u.%u Hz  ·  %s",m.soundHz/10,m.soundHz%10,std::abs(cents)<=5?"准":cents>0?"偏高":"偏低");setLabel(soundSub,text);}
        else{setLabel(soundBig,"--");setLabel(soundSub,"对着麦克风发出持续的音");}
        snprintf(text,sizeof(text),"标准音 A4 = %u Hz",t.a4);setLabel(soundInfo,text);
    }else{
        snprintf(text,sizeof(text),"%u",t.bpm);setLabel(soundBig,text);setTextColor(soundBig,lv_color_hex(m.soundRunning?kAccent:kText),0);
        setLabel(soundSub,m.soundRunning?"BPM  ·  4/4 拍进行中":"BPM  ·  确认开始");setLabel(soundInfo,"音量跟随系统设置");
    }
    lv_obj_invalidate(soundView);
}
// 移动监控: the activity level in large digits and what it means, the last 30 s as bars against the
// sensitivity line, then when movement was last seen and how well the router is heard.
lv_obj_t *motionRoot,*motionBig,*motionState,*motionView,*motionInfo,*motionLink,*motionBell,*motionAlarm;
constexpr uint32_t kQuiet=0x22c55e,kMoving=0xff6b6b;
void drawMotion(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const int limit=m.tools.motionLimit(),top=4,bottom=96;
    p.rect(0,bottom+1,223,bottom+1,kBorder);
    for(unsigned i=0;i<m.motionHistoryCount;++i){
        // Newest on the right edge.
        const unsigned slot=60-m.motionHistoryCount+i;const int x=slot*224/60,level=m.motionHistory[i];
        p.rect(x,bottom-std::max(1,level*(bottom-top)/100),x+2,bottom,level>limit?kMoving:kAccent,1);
    }
    const int y=bottom-limit*(bottom-top)/100;
    for(int x=0;x<224;x+=8)p.rect(x,y,x+3,y,kWarn);
    p.text("30 秒前",0,bottom+4,100,kMuted);p.text("现在",124,bottom+4,223,kMuted,LV_TEXT_ALIGN_RIGHT);
}
void initMotion(lv_obj_t* screen){
    motionRoot=lv_obj_create(screen);box(motionRoot,0,35,240,257,kBg);
    motionBig=label(motionRoot,0,4,240,&lv_font_montserrat_48,kText);setTextAlign(motionBig,LV_TEXT_ALIGN_CENTER,0);
    motionState=textRow(motionRoot,0,60,240,kMuted);setTextAlign(motionState,LV_TEXT_ALIGN_CENTER,0);
    // The alarm setting, top right: an icon and its name.
    motionBell=label(motionRoot,192,10,40,&lv_font_montserrat_20,kMuted);setTextAlign(motionBell,LV_TEXT_ALIGN_CENTER,0);
    motionAlarm=label(motionRoot,188,34,48,LV_FONT_DEFAULT,kMuted);setTextAlign(motionAlarm,LV_TEXT_ALIGN_CENTER,0);
    motionView=lv_obj_create(motionRoot);box(motionView,8,88,224,124,kBg);lv_obj_add_event_cb(motionView,drawMotion,LV_EVENT_DRAW_MAIN,nullptr);
    motionInfo=textRow(motionRoot,12,212,216,kText);motionLink=textRow(motionRoot,12,234,216,kMuted);
}
void renderMotion(const Model& m){
    drawModel=&m;char text[96];const bool live=m.motionPhase==2||m.motionPhase==3;
    snprintf(text,sizeof(text),"%u",m.motionLevel);setLabel(motionBig,live?text:"--");
    setTextColor(motionBig,lv_color_hex(m.motionPhase==3?kMoving:m.motionPhase==2?kQuiet:kMuted),0);
    if(m.motionPhase==1)snprintf(text,sizeof(text),"正在校准，请保持安静 %u 秒",m.motionCalLeft);
    setLabel(motionState,m.motionPhase==0?"WiFi 未连接，无法监控":m.motionPhase==1?text:m.motionPhase==2?"安静 · 活动度":m.motionPhase==3?"有人移动":"收不到路由器的数据");
    setTextColor(motionState,lv_color_hex(m.motionPhase==3?kMoving:m.motionPhase==4?kWarn:kMuted),0);
    const long long since=static_cast<long long>(time(nullptr))-m.motionLastAt;
    if(!m.motionLastAt||time(nullptr)<1700000000)snprintf(text,sizeof(text),"还没有检测到移动");
    else if(since<60)snprintf(text,sizeof(text),"上次移动 %lld 秒前 · 共 %u 次",since,m.motionEvents);
    else if(since<3600)snprintf(text,sizeof(text),"上次移动 %lld 分钟前 · 共 %u 次",since/60,m.motionEvents);
    else snprintf(text,sizeof(text),"上次移动 %lld 小时前 · 共 %u 次",since/3600,m.motionEvents);
    setLabel(motionInfo,text);
    {const unsigned alarm=m.tools.alarm();const uint32_t color=alarm==2?kWarn:alarm?kAccent:kMuted;
     setLabel(motionBell,alarm?LV_SYMBOL_BELL:LV_SYMBOL_MUTE);setTextColor(motionBell,lv_color_hex(color),0);
     setLabel(motionAlarm,alarm==2?"连续":alarm?"一次":"静音");setTextColor(motionAlarm,lv_color_hex(color),0);}
    if(m.motionRate)snprintf(text,sizeof(text),"灵敏度 %u · 每秒收到 %u 包",m.tools.sense(),m.motionRate);
    else snprintf(text,sizeof(text),"灵敏度 %u",m.tools.sense());
    setLabel(motionLink,text);
    lv_obj_invalidate(motionView);
}
// 天气预报: four tabs over one custom-drawn panel: a row per day (name, sky, chance of rain, low~high), the next
// 24 hours as a temperature curve over rain bars, the air (China AQI now and as bars for 24 hours) and the sun,
// or the warnings in force.
lv_obj_t *fcRoot,*fcTabs[4],*fcTabText[4],*fcMeta,*fcView;
uint32_t skyColor(int code){
    switch(weatherIcon(code)){
    case WeatherIcon::Sun: return 0xffc861;
    case WeatherIcon::PartlyCloudy: return 0xffe39a;
    case WeatherIcon::Cloud: return 0x9bbbd1;
    case WeatherIcon::Fog: return 0x7c8b96;
    case WeatherIcon::Rain: return 0x00baf2;
    case WeatherIcon::Snow: return 0xeaf4ff;
    case WeatherIcon::Thunder: return 0xb59cff;
    default: return kMuted;
    }
}
uint32_t aqiColor(int aqi){return aqi<0?kMuted:aqi<=50?0x3ccf6e:aqi<=100?0xf5d33d:aqi<=150?0xff9a3c:aqi<=200?0xf04848:aqi<=300?0xb36ae2:0xc0506a;}
void drawAir(Painter& p,const WeatherForecast& f){
    char text[48];
    p.rect(0,0,223,74,kPanel,10);
    if(f.airHours&&f.aqi[0]>=0){
        const int now=f.aqi[0];const uint32_t color=aqiColor(now);
        p.text("AQI",10,6,60,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
        snprintf(text,sizeof(text),"%d",now);p.text(text,8,18,110,color,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_48);
        p.text(aqiLevel(now),112,6,216,color);
        // PM2.5 and PM10 now, in µg/m³.
        const auto pm=[&](int y,const char* name,uint16_t v){p.text(name,112,y,170,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
            if(v!=0xffff)snprintf(text,sizeof(text),"%u",v);else snprintf(text,sizeof(text),"--");p.text(text,160,y,214,kText,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);};
        pm(32,"PM2.5",f.pm25);pm(52,"PM10",f.pm10);
        // The next 24 hours as bars on y 96-128, each in its level's colour, 100 at least as tall as the box.
        int top=100;for(unsigned i=0;i<f.airHours;++i)top=std::max<int>(top,f.aqi[i]);
        p.text("未来 24 小时",4,78,120,kMuted);
        if(now>50){snprintf(text,sizeof(text),"首要 %s",aqiPollutant(f.primary));p.text(text,110,78,221,kMuted,LV_TEXT_ALIGN_RIGHT);}
        for(unsigned i=0;i<f.airHours;++i){if(f.aqi[i]<0)continue;const int x=4+i*9,h=std::max(2,f.aqi[i]*32/top);p.rect(x,128-h,x+5,128,aqiColor(f.aqi[i]),2);}
        for(unsigned i=0;i<f.airHours;i+=6){if(i)snprintf(text,sizeof(text),"%u时",(f.hour0+i)%24);else snprintf(text,sizeof(text),"现在");p.text(text,i?int(i)*9-10:0,130,int(i)*9+(i?26:40),kMuted,i?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT);}
    }else p.text("空气质量暂不可用",0,26,223,kMuted,LV_TEXT_ALIGN_CENTER);
    // The sun: today's sunrise and sunset, then the highest UV index and its WHO level.
    p.rect(0,152,223,197,kPanel,10);
    char rise[8]="--:--",set[8]="--:--";
    if(f.sunrise<1440){snprintf(rise,sizeof(rise),"%02u:%02u",f.sunrise/60,f.sunrise%60);}
    if(f.sunset<1440){snprintf(set,sizeof(set),"%02u:%02u",f.sunset/60,f.sunset%60);}
    p.rect(10,160,17,167,0xffc861,4);snprintf(text,sizeof(text),"日出 %s",rise);p.text(text,22,155,112,kText);
    p.rect(118,160,125,167,0xff8a5c,4);snprintf(text,sizeof(text),"日落 %s",set);p.text(text,130,155,218,kText);
    if(f.uv<=250){const unsigned uv=f.uv;snprintf(text,sizeof(text),"紫外线最高 %u.%u  ·  %s",uv/10,uv%10,uv<30?"低":uv<60?"中等":uv<80?"高":uv<110?"很高":"极高");}
    else snprintf(text,sizeof(text),"紫外线 --");
    p.text(text,10,176,218,kMuted);
}
// When a warning was issued, short: the time today, else the day ("MM/DD HH:MM" in).
const char* issuedShort(const WeatherWarning& w,char (&out)[8]){
    char today[8];const time_t now=time(nullptr);tm local{};localtime_r(&now,&local);strftime(today,sizeof(today),"%m/%d",&local);
    snprintf(out,sizeof(out),"%.5s",strlen(w.time)<11?"":strncmp(w.time,today,5)?w.time:w.time+6);return out;
}
// A row per warning, highest first: the kind in its level's colour and when, then who issued it.
void drawWarnings(Painter& p,const WeatherForecast& f){
    char text[64],when[8];
    if(f.warnings==0xff){p.text("预警暂不可用",0,40,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("中央气象台 · 仅限中国内地",0,64,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    if(!f.warnings){p.rect(0,0,223,74,kPanel,10);p.text("当前没有生效的预警",0,14,223,0x3ccf6e,LV_TEXT_ALIGN_CENTER);p.text("本市及省气象台发布",0,40,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    for(unsigned i=0;i<f.warnings;++i){
        const auto& w=f.warning[i];const int y=i*49;const uint32_t color=kWarningColors[w.level%4];
        p.rect(0,y,223,y+44,kPanel,8);p.rect(0,y,5,y+44,color,3);
        snprintf(text,sizeof(text),"%s%s预警",w.type,kWarningLevels[w.level%4]);p.text(text,14,y+4,170,color);
        p.text(issuedShort(w,when),160,y+4,216,kMuted,LV_TEXT_ALIGN_RIGHT);
        if(w.places>1)snprintf(text,sizeof(text),"%s 等 %u 地",w.where,w.places);else snprintf(text,sizeof(text),"%s",w.where);
        p.text(text,14,y+24,216,kMuted);
    }
}
// Where the part of s that fits in `room` pixels ends (lv_draw_label does not clip to its area).
const char* lineEnd(const char* s,int room,const lv_font_t* font){
    const char* start=s;int width=0;
    while(*s){
        const auto c=static_cast<unsigned char>(*s);const unsigned n=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        uint32_t code=n==1?c:c&(0xff>>(n+1));for(unsigned k=1;k<n&&s[k];++k)code=code<<6|(s[k]&0x3f);
        const int glyph=lv_font_get_glyph_width(font,code,0);
        if(s!=start&&width+glyph>room)break;
        width+=glyph;s+=n;
    }
    return s;
}
// lineEnd, but an English word that would be cut moves whole to the next line when the line has a space to break at.
const char* wrapEnd(const char* s,int room){
    const char* end=lineEnd(s,room);
    const auto latin=[](char c){return static_cast<unsigned char>(c)<0x80&&c!=' ';};
    if(!*end||end==s||!latin(*end)||!latin(end[-1]))return end;
    for(const char* at=end;at>s;--at)if(at[-1]==' ')return at;
    return end;
}
// Text wrapped to the panel's width, 20 px a line from y, at most `most` lines; a longer text ends in "…".
// words: English wraps between words (wrapEnd).
void paragraph(Painter& p,const char* s,int y,unsigned most,uint32_t color,int x1=0,int x2=223,bool words=false){
    for(unsigned line=0;*s&&line<most;++line){
        const bool last=line+1==most;const int room=x2-x1+1-(last?lv_font_get_glyph_width(LV_FONT_DEFAULT,0x2026,0):0);
        if(words)while(*s==' ')++s;
        const char* start=s;s=words&&!last?wrapEnd(s,room):lineEnd(s,room);
        char text[100];fitText(text,sizeof(text)-3,start,s-start);if(last&&*s)strcat(text,"…");
        p.text(text,x1,y+int(line)*20,x2,color);
    }
}
// One warning's text (they run 40-120 characters, eight lines hold about 115).
void drawWarningText(Painter& p,const WeatherForecast& f,unsigned i){
    const auto& w=f.warning[i];const uint32_t color=kWarningColors[w.level%4];char text[48],when[8];
    p.rect(0,0,223,24,kPanel,6);p.rect(0,0,5,24,color,3);
    snprintf(text,sizeof(text),"%s%s预警",w.type,kWarningLevels[w.level%4]);p.text(text,14,2,170,color);
    p.text(issuedShort(w,when),160,2,216,kMuted,LV_TEXT_ALIGN_RIGHT);
    if(f.detailFor!=i){p.text("正在获取预警原文",0,70,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    paragraph(p,f.detail,32,8,kText);
}
void drawForecast(lv_event_t* e){
    const auto& m=*drawModel;const auto& f=m.page.forecast;auto p=painter(e);char text[24];
    if(m.forecastView==3){
        if(!f.days)return;
        if(m.forecastWarn&&f.warnings!=0xff&&m.forecastWarn<=f.warnings)drawWarningText(p,f,m.forecastWarn-1);else drawWarnings(p,f);
        return;
    }
    if(m.forecastView==2){drawAir(p,f);return;}
    if(!m.forecastView){
        const time_t now=time(nullptr);tm today{};localtime_r(&now,&today);
        static const char* const week[]{"周日","周一","周二","周三","周四","周五","周六"};
        for(unsigned i=0;i<f.days;++i){
            const auto& d=f.day[i];const int y=i*27;
            p.rect(0,y,223,y+24,i?kPanel:kBorder,6);
            if(i<2)snprintf(text,sizeof(text),"%s",i?"明天":"今天");else if(m.clockValid)snprintf(text,sizeof(text),"%s",week[(today.tm_wday+i)%7]);else snprintf(text,sizeof(text),"第%u天",i+1);
            p.text(text,6,y+3,48,i?kMuted:kText);
            const uint32_t sky=skyColor(d.code);p.rect(50,y+8,59,y+17,sky,5);p.text(weatherName(d.code),64,y+3,130,sky);
            if(d.rain<=100){snprintf(text,sizeof(text),"%u%%",d.rain);p.text(text,124,y+3,158,d.rain>=50?0x00baf2:kMuted,LV_TEXT_ALIGN_RIGHT);}
            snprintf(text,sizeof(text),"%d~%d°",d.low,d.high);p.text(text,162,y+3,216,kText,LV_TEXT_ALIGN_RIGHT);
        }
        return;
    }
    if(!f.hours)return;
    int low=99,high=-99;for(unsigned i=0;i<f.hours;++i){low=std::min<int>(low,f.hour[i].temp);high=std::max<int>(high,f.hour[i].temp);}
    // The curve from y 24 (warmest) to 96 (coldest); rain bars stand on y 156.
    const auto x=[&](unsigned i){return 20+static_cast<int>(i)*184/23;};
    const auto y=[&](unsigned i){return 96-(f.hour[i].temp-low)*72/std::max(1,high-low);};
    p.rect(0,156,223,156,kBorder);
    for(unsigned i=0;i<f.hours;++i){
        const auto& h=f.hour[i];
        if(h.rain&&h.rain<=100)p.rect(x(i)-3,156-h.rain*40/100,x(i)+3,155,0x00baf2,2);
        if(i)p.line(x(i-1),y(i-1),x(i),y(i),kAccent);
    }
    // Labels every six hours and at the last one.
    for(unsigned i=0;i<f.hours;i=i+6<f.hours||i+1==f.hours?i+6:f.hours-1){
        const auto& h=f.hour[i];p.rect(x(i)-4,y(i)-4,x(i)+4,y(i)+4,skyColor(h.code),4);
        snprintf(text,sizeof(text),"%d°",h.temp);p.text(text,x(i)-18,y(i)-24,x(i)+18,kText,LV_TEXT_ALIGN_CENTER);
        if(i)snprintf(text,sizeof(text),"%u时",(f.hour0+i)%24);else snprintf(text,sizeof(text),"现在");
        p.text(text,x(i)-18,162,x(i)+18,kMuted,LV_TEXT_ALIGN_CENTER);
    }
    p.rect(0,186,7,193,0x00baf2,2);p.text("降水概率",12,181,80,kMuted);
    snprintf(text,sizeof(text),"温度 %d~%d°",low,high);p.text(text,80,181,223,kMuted,LV_TEXT_ALIGN_RIGHT);
}
void initForecast(lv_obj_t* screen){
    fcRoot=lv_obj_create(screen);box(fcRoot,0,35,240,257,kBg);
    initTabs(fcRoot,fcTabs,fcTabText,{"7 天","24 小时","空气","预警"});
    fcMeta=textRow(fcRoot,12,32,216,kMuted);
    fcView=lv_obj_create(fcRoot);box(fcView,8,56,224,198,kBg);lv_obj_add_event_cb(fcView,drawForecast,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderForecast(const Model& m){
    drawModel=&m;const auto& f=m.page.forecast;char text[128];
    // Warnings in force colour their tab while another is shown (one write each: style writes repaint).
    renderTabs(fcTabs,fcTabText,3,m.forecastView);const bool warnOn=m.forecastView==3,warned=f.warnings&&f.warnings!=0xff;
    setBgColor(fcTabs[3],lv_color_hex(warnOn?kAccent:kPanel),0);
    setTextColor(fcTabText[3],lv_color_hex(warnOn?kBg:warned?kWarningColors[f.warning[0].level%4]:kMuted),0);
    if(!f.days)snprintf(text,sizeof(text),"%s",m.livePhase==LivePhase::Error?m.liveState:"正在获取天气预报");
    else if(m.forecastView==3&&m.forecastWarn&&f.warnings!=0xff&&m.forecastWarn<=f.warnings&&m.livePhase!=LivePhase::Error){
        // Which warning and who issued it.
        snprintf(text,sizeof(text),"%u/%u  ·  %s",m.forecastWarn,f.warnings,f.warning[m.forecastWarn-1].where);
    }
    else snprintf(text,sizeof(text),"%s  ·  %s",f.city,m.livePhase==LivePhase::Error?m.liveState:m.livePhase==LivePhase::Loading?"正在刷新":m.forecastView==3?"气象预警":m.forecastView==2?"空气质量 · 国标":m.forecastView?"未来 24 小时":"未来 7 天");
    setLabel(fcMeta,text);lv_obj_invalidate(fcView);
}
// 白噪音: a row per sound and the sleep-timer row; the playing sound shows moving bars.
lv_obj_t *noiseRoot,*noiseView;
void drawNoise(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);char text[64];
    constexpr unsigned sounds=std::size(kNoiseSounds);
    const uint64_t now=esp_timer_get_time()/1000000;
    for(unsigned i=0;i<=sounds;++i){
        const int y=i*34;const bool on=i==m.noiseRow,playing=i<sounds&&m.noisePlaying&&m.noiseKind==i;
        p.rect(0,y,223,y+29,on?kAccent:kPanel,8);
        if(i==sounds){
            p.text("定时关闭",12,y+5,120,on?kBg:kText);
            if(m.noisePlaying&&m.noiseUntil>now){const unsigned left=m.noiseUntil-now;snprintf(text,sizeof(text),"剩 %u:%02u",left/60,left%60);}
            else if(m.noiseTimer)snprintf(text,sizeof(text),"%u 分钟",m.noiseTimer);
            else snprintf(text,sizeof(text),"不定时");
            p.text(text,100,y+5,212,on?kBg:kMuted,LV_TEXT_ALIGN_RIGHT);
            continue;
        }
        p.text(kNoiseSounds[i].name,12,y+5,120,on?kBg:kText);
        if(!playing)continue;
        // Three bars that move while it plays (the page redraws every second).
        for(unsigned b=0;b<3;++b){const int h=4+static_cast<int>((now*7+b*5)%13);p.rect(196+b*7,y+22-h,200+b*7,y+22,on?kBg:kAccent,1);}
        p.text("播放中",110,y+5,188,on?kBg:kAccent,LV_TEXT_ALIGN_RIGHT);
    }
    p.text(m.noiseRow>=sounds?"时间到后声音渐弱停止":kNoiseSounds[m.noiseRow].note,4,210,223,kMuted);
    snprintf(text,sizeof(text),"音量 %u%%  ·  播放时可以息屏",m.volume);p.text(text,4,232,223,kMuted);
}
void initNoise(lv_obj_t* screen){
    noiseRoot=lv_obj_create(screen);box(noiseRoot,0,35,240,257,kBg);
    noiseView=lv_obj_create(noiseRoot);box(noiseView,8,4,224,253,kBg);lv_obj_add_event_cb(noiseView,drawNoise,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderNoise(const Model& m){drawModel=&m;lv_obj_invalidate(noiseView);}
// 闹钟: a 录音 row over the alarms (time, repeat, when it rings next); an alarm's clock large with its four repeat
// modes; the listening screen. Ringing: the clock on a flashing card.
lv_obj_t *alarmRoot,*alarmView;
constexpr const char* kAlarmModes[]{"关闭","每天","工作日","一次"};
// "今天" / "明天" / "后天" / "11月3日周二" for a ring.
void alarmDay(char* out,size_t size,time_t at,time_t clock){
    static const char* const week[]{"周日","周一","周二","周三","周四","周五","周六"};
    tm n{},t{};localtime_r(&at,&n);localtime_r(&clock,&t);
    const int days=dayNumber({n.tm_year+1900,n.tm_mon+1,n.tm_mday})-dayNumber({t.tm_year+1900,t.tm_mon+1,t.tm_mday});
    if(days<3)snprintf(out,size,"%s",days==0?"今天":days==1?"明天":"后天");else snprintf(out,size,"%d月%d日%s",n.tm_mon+1,n.tm_mday,week[n.tm_wday]);
}
void drawListening(Painter& p,const Model& m,uint64_t now,const char* listenNote,const char* recognizeNote,const char* listenTitle="正在听，说完按确认");
void drawAlarmClock(Painter& p,const Model& m,const AlarmPrefs& a,time_t clock){
    char text[64];
    snprintf(text,sizeof(text),"%02u:%02u",a.hour,a.minute);p.text(text,0,2,223,a.mode?kText:kMuted,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_48);
    for(unsigned i=0;i<4;++i){
        const int x=i*56;const bool on=i==a.mode;
        p.rect(x,60,x+54,86,on?kAccent:kPanel,13);p.text(kAlarmModes[i],x,63,x+54,on?kBg:kMuted,LV_TEXT_ALIGN_CENTER);
    }
    const time_t next=m.clockValid?nextAlarm(a,clock):0;
    if(next){
        char day[24];alarmDay(day,sizeof(day),next,clock);
        snprintf(text,sizeof(text),"下次响铃：%s %02d:%02d",day,a.hour,a.minute);p.text(text,0,100,223,kText,LV_TEXT_ALIGN_CENTER);
        const long left=(next-clock+59)/60;
        if(left>=60)snprintf(text,sizeof(text),"还有 %ld 小时 %ld 分钟",left/60,left%60);else snprintf(text,sizeof(text),"还有 %ld 分钟",left);
        p.text(text,0,124,223,kMuted,LV_TEXT_ALIGN_CENTER);
    }else p.text(a.mode?"等待网络校时":"这个闹钟已关闭",0,100,223,kMuted,LV_TEXT_ALIGN_CENTER);
    p.rect(0,162,223,248,kPanel,10);
    snprintf(text,sizeof(text),"·  第 %u 个，共 %u 个",m.alarmIndex,m.alarms.count);p.text(text,10,170,220,kMuted);
    p.text("·  工作日按法定假日和调休",10,196,220,kMuted);
    p.text("·  息屏和睡眠时也会响",10,222,220,kMuted);
}
void drawAlarm(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);char text[96];const auto& list=m.alarms;
    const time_t clock=time(nullptr);tm t{};localtime_r(&clock,&t);
    if(m.alarmRinging){
        const bool flash=clock%2;
        p.rect(0,0,223,250,flash?0x5a1d24:kPanel,14);
        snprintf(text,sizeof(text),"%02d:%02d",t.tm_hour,t.tm_min);p.text(text,0,34,223,kText,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_48);
        p.text("闹钟响了",0,100,223,flash?kWarn:kText,LV_TEXT_ALIGN_CENTER);
        p.rect(24,146,199,182,kAccent,18);p.text("确认  关闭",24,154,199,kBg,LV_TEXT_ALIGN_CENTER);
        p.rect(24,194,199,230,kBorder,18);p.text("上下  贪睡 5 分钟",24,202,199,kText,LV_TEXT_ALIGN_CENTER);
        return;
    }
    if(m.voiceBusy()){drawListening(p,m,esp_timer_get_time()/1000000,"例：工作日早上 7 点半","说「每天」「工作日」会重复，否则响一次");return;}
    if(m.alarmEdit&&m.alarmIndex&&m.alarmIndex<=list.count){drawAlarmClock(p,m,list.items[m.alarmIndex-1],clock);return;}
    // The top line: the delete question, a notice, the snooze, or the next ring.
    const time_t next=m.clockValid?nextAlarm(list,clock):0;
    if(m.alarmArmed){p.rect(0,0,223,21,0xff5a5a,6);p.text("再长按下键删除选中的闹钟",0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)p.text(m.lastNotice,0,0,223,kWarn);
    else if(m.alarmSnooze){const time_t at=m.alarmSnooze;tm s{};localtime_r(&at,&s);snprintf(text,sizeof(text),"贪睡中，%02d:%02d 再响 · 确认取消",s.tm_hour,s.tm_min);p.text(text,0,0,223,kWarn);}
    else if(next){
        char day[24];alarmDay(day,sizeof(day),next,clock);tm n{};localtime_r(&next,&n);const long left=(next-clock+59)/60;
        if(left>=60)snprintf(text,sizeof(text),"下次 %s %02d:%02d · %ld 小时后",day,n.tm_hour,n.tm_min,left/60);else snprintf(text,sizeof(text),"下次 %s %02d:%02d · %ld 分钟后",day,n.tm_hour,n.tm_min,left);
        p.text(text,0,0,223,kText);
    }else p.text(list.count?(m.clockValid?"闹钟都已关闭":"等待网络校时"):"还没有闹钟",0,0,223,kMuted);
    // The 录音 row.
    const bool rec=!m.alarmIndex;
    p.rect(0,24,223,62,rec?kBorder:kPanel,8);p.rect(8,29,36,57,rec?kWarn:kBorder,14);
    p.text(ICON_MIC,8,32,36,0xffffff,LV_TEXT_ALIGN_CENTER,&passport_icon_font_20);
    p.text("按确认说闹钟时间",46,24,220,kText);p.text("例：工作日早上 7 点半",46,42,220,kMuted);
    if(!list.count){p.text("说「明早 7 点」或「每天 6 点 50」",0,120,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("最多 5 个闹钟",0,146,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);return;}
    for(unsigned i=0;i<list.count;++i){
        const auto& a=list.items[i];const int y=68+int(i)*37;const bool picked=m.alarmIndex==i+1;
        p.rect(0,y,223,y+34,picked?(m.alarmArmed?0x5a1d24:kBorder):kPanel,8);
        snprintf(text,sizeof(text),"%02u:%02u",a.hour,a.minute);p.text(text,10,y+5,90,a.mode?kText:kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_24);
        if(const time_t at=m.clockValid?nextAlarm(a,clock):0){char day[24];alarmDay(day,sizeof(day),at,clock);p.text(day,92,y+7,160,kMuted);}
        p.rect(160,y+6,216,y+28,a.mode?kAccent:kPanel,11);p.text(kAlarmModes[a.mode%4],160,y+7,216,a.mode?kBg:kMuted,LV_TEXT_ALIGN_CENTER);
    }
}
void initAlarm(lv_obj_t* screen){
    alarmRoot=lv_obj_create(screen);box(alarmRoot,0,35,240,257,kBg);
    alarmView=lv_obj_create(alarmRoot);box(alarmView,8,4,224,253,kBg);lv_obj_add_event_cb(alarmView,drawAlarm,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderAlarm(const Model& m){drawModel=&m;lv_obj_invalidate(alarmView);}
// 自选股. The list: a row per security (name, market and code, price, change) with its change in a red or green
// pill (Chinese colours: red up). The detail: the price large, the change and turnover, today's minute line
// against the previous close (or 30 daily candles), then open, high, previous close and low.
lv_obj_t *stockRoot,*stockView;
constexpr uint32_t kRise=0xf04848,kFall=0x1fb862;
uint32_t stockColor(float change){return !std::isfinite(change)||change==0?kMuted:change>0?kRise:kFall;}
void stockPrice(char* out,size_t size,float v,unsigned decimals){if(std::isfinite(v))snprintf(out,size,"%.*f",static_cast<int>(std::min(decimals,4u)),double(v));else snprintf(out,size,"--");}
void drawStockList(Painter& p,const Model& m){
    char text[160];
    if(m.stockArmed){p.rect(0,0,223,21,0xff5a5a,6);snprintf(text,sizeof(text),"再长按下键删除%s",m.page.stocks.rows[m.stockIndex%m.stockCount].name);p.text(text,0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)snprintf(text,sizeof(text),"%s",m.lastNotice);
    else if(m.livePhase==LivePhase::Error)snprintf(text,sizeof(text),"%s",m.liveState);
    else if(m.stockUpdated){const time_t at=m.stockUpdated;tm t{};localtime_r(&at,&t);snprintf(text,sizeof(text),"自选 %u 只  ·  %02d:%02d:%02d 更新",m.stockCount,t.tm_hour,t.tm_min,t.tm_sec);}
    else snprintf(text,sizeof(text),"自选 %u 只",m.stockCount);
    if(!m.stockArmed)p.text(text,4,0,223,*m.lastNotice?kWarn:kMuted);
    const unsigned first=m.stockIndex>5?m.stockIndex-5:0;
    for(unsigned i=first;i<m.stockCount&&i<first+6;++i){
        const auto& r=m.page.stocks.rows[i];const int y=24+(i-first)*38;
        p.rect(0,y,223,y+35,i==m.stockIndex?kBorder:kPanel,8);
        p.text(r.name,10,y+1,96,kText);
        snprintf(text,sizeof(text),"%s %s",r.tag,r.code);p.text(text,10,y+19,96,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
        stockPrice(text,sizeof(text),r.price,r.decimals);p.text(text,92,y+1,156,kText,LV_TEXT_ALIGN_RIGHT);
        if(std::isfinite(r.change)){snprintf(text,sizeof(text),"%+.*f",static_cast<int>(std::min<unsigned>(r.decimals,4)),double(r.change));p.text(text,92,y+19,156,stockColor(r.change),LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);}
        p.rect(162,y+6,219,y+29,stockColor(r.pct),6);
        if(std::isfinite(r.pct))snprintf(text,sizeof(text),"%+.2f%%",double(r.pct));else snprintf(text,sizeof(text),"--");
        p.text(text,162,y+9,219,0xffffff,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);
    }
}
void drawStockDetail(Painter& p,const Model& m){
    char text[64];const auto& r=m.page.stocks.rows[m.stockIndex%m.stockCount];const unsigned decimals=std::min<unsigned>(r.decimals,4);
    p.text(r.name,4,0,170,kText);
    snprintf(text,sizeof(text),"%s %s",r.tag,r.code);p.text(text,120,3,219,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    const uint32_t color=stockColor(r.change);
    stockPrice(text,sizeof(text),r.price,decimals);p.text(text,2,20,223,color,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_48);
    if(std::isfinite(r.change)&&std::isfinite(r.pct))snprintf(text,sizeof(text),"%+.*f   %+.2f%%",static_cast<int>(decimals),double(r.change),double(r.pct));else snprintf(text,sizeof(text),"--");
    p.text(text,4,70,150,color);
    if(std::isfinite(r.amount)&&r.amount>0){if(r.amount>=1e8f)snprintf(text,sizeof(text),"额 %.1f亿",double(r.amount)/1e8);else snprintf(text,sizeof(text),"额 %.0f万",double(r.amount)/1e4);p.text(text,120,70,219,kMuted,LV_TEXT_ALIGN_RIGHT);}
    // The chart: 94-192, its axis under it.
    constexpr int top=94,bottom=192;
    p.rect(0,top,223,bottom,kPanel,8);
    p.text(m.stockDays?"日K · 30 天":"分时",130,top+2,218,kMuted,LV_TEXT_ALIGN_RIGHT);
    const auto& c=m.page.stocks.chart;
    const bool ready=m.chartFor==m.stockIndex&&m.chartDays==m.stockDays&&m.chartCount;
    if(!ready)p.text(m.livePhase==LivePhase::Error?m.liveState:"正在加载走势…",0,top+38,223,kMuted,LV_TEXT_ALIGN_CENTER);
    else if(!m.chartDays){
        // Minutes: the previous close across the middle, the price around it on a symmetric scale.
        const auto value=[&](unsigned i){return m.chartLow+(m.chartHigh-m.chartLow)*c[i]/999.f;};
        const float prev=std::isfinite(r.prev)&&r.prev>0?r.prev:value(0);
        const float range=std::max({std::fabs(m.chartHigh-prev),std::fabs(m.chartLow-prev),prev*0.002f});
        constexpr int mid=(top+bottom)/2+6,half=(bottom-top)/2-14;
        const auto y=[&](float v){return mid-static_cast<int>(std::lround((v-prev)/range*half));};
        const auto x=[&](unsigned i){return 4+static_cast<int>(i*215/std::max(1,m.chartTotal-1));};
        for(int dx=4;dx<219;dx+=8)p.rect(dx,mid,dx+3,mid,kBorder);
        const uint32_t ink=value(m.chartCount-1)>=prev?kRise:kFall;
        for(unsigned i=1;i<m.chartCount;++i)p.line(x(i-1),y(value(i-1)),x(i),y(value(i)),ink,2);
        char price[16];stockPrice(price,sizeof(price),prev+range,decimals);snprintf(text,sizeof(text),"%s  %+.2f%%",price,double(range/prev*100));p.text(text,4,top+2,128,kRise,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
        stockPrice(price,sizeof(price),prev-range,decimals);snprintf(text,sizeof(text),"%s  %+.2f%%",price,double(-range/prev*100));p.text(text,4,bottom-17,128,kFall,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
    }else{
        // Days: open, close, high, low per candle; red when it closed at or above its open.
        const unsigned days=m.chartCount/4;const float slot=215.f/std::max(days,1u);
        const auto y=[&](int v){return bottom-6-v*(bottom-top-26)/999;};
        for(unsigned d=0;d<days;++d){
            const int open=c[4*d],close=c[4*d+1],high=c[4*d+2],low=c[4*d+3];const int x=4+static_cast<int>(slot*d+slot/2);
            const uint32_t ink=close>=open?kRise:kFall;
            p.rect(x,y(high),x,y(low),ink);p.rect(x-2,y(std::max(open,close)),x+2,y(std::min(open,close)),ink);
        }
        stockPrice(text,sizeof(text),m.chartHigh,decimals);p.text(text,4,top+2,128,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
        stockPrice(text,sizeof(text),m.chartLow,decimals);p.text(text,4,bottom-17,128,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
    }
    if(ready){
        // The default font: the labels can be Chinese (美东时间, 次日06:00).
        p.text(m.chartFrom,2,bottom+1,80,kMuted);
        p.text(m.chartMid,60,bottom+1,164,kMuted,LV_TEXT_ALIGN_CENTER);
        p.text(m.chartTo,140,bottom+1,221,kMuted,LV_TEXT_ALIGN_RIGHT);
    }
    const auto stat=[&](int x,int y,const char* name,float v){p.text(name,x,y,x+40,kMuted);char s[16];stockPrice(s,sizeof(s),v,decimals);p.text(s,x+38,y,x+106,kText);};
    stat(4,212,"今开",r.open);stat(116,212,"最高",r.high);stat(4,232,"昨收",r.prev);stat(116,232,"最低",r.low);
}
void drawListening(Painter& p,const Model& m,uint64_t now,const char* listenNote,const char* recognizeNote,const char* listenTitle);
void drawStock(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);
    if(m.voiceBusy()){drawListening(p,m,esp_timer_get_time()/1000000,"例：添加比亚迪、英伟达","识别后在网上查找并加入自选");return;}
    if(!m.stockCount){p.text(m.livePhase==LivePhase::Error?m.liveState:"正在获取行情",0,100,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    if(m.stockDetail)drawStockDetail(p,m);else drawStockList(p,m);
}
void initStock(lv_obj_t* screen){
    stockRoot=lv_obj_create(screen);box(stockRoot,0,35,240,257,kBg);
    stockView=lv_obj_create(stockRoot);box(stockView,8,2,224,255,kBg);lv_obj_add_event_cb(stockView,drawStock,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderStock(const Model& m){drawModel=&m;lv_obj_invalidate(stockView);}
// 台风路径: the picked typhoon's track on a map of the region (land filled, a grid every 5 or 10 degrees, the setup
// page's city marked), the official forecast dashed ahead of it; under the map its grade, wind, pressure, motion and
// distance, or for one that has ended its peak.
lv_obj_t *tyRoot,*tyView;
constexpr int kMapTop=22,kMapBottom=171;
constexpr uint32_t kLand=0x24453b,kGrid=0x16384b;
// "10/26 11:00" in local time.
// "45 m/s · 14 级"; above force 17 "17 级以上".
void force(char* out,size_t size,unsigned ms){const unsigned f=beaufort(ms);snprintf(out,size,f>17?"%u m/s · 17 级以上":"%u m/s · %u 级",ms,f);}
void fixTime(uint32_t at,char* out,size_t size){const time_t t=at;tm local{};localtime_r(&t,&local);strftime(out,size,"%m/%d %H:%M",&local);}
// An ended typhoon still carries its last fix's forecast; only an active one's is drawn.
void drawTyphoonMap(Painter& p,const TyphoonView& t,bool active){
    char text[48];
    // Fit the track, the forecast and the city when it is in the region.
    double w0=999,e0=-999,s0=999,n0=-999;
    const auto take=[&](int lon,int lat){w0=std::min(w0,lon/10.);e0=std::max(e0,lon/10.);s0=std::min(s0,lat/10.);n0=std::max(n0,lat/10.);};
    for(unsigned i=0;i<t.points;++i)take(t.track[i].lon,t.track[i].lat);
    const unsigned ahead=active?t.ahead:0;
    for(unsigned i=0;i<ahead;++i)take(t.forecast[i].lon,t.forecast[i].lat);
    const bool city=t.cityLat!=INT16_MIN&&t.cityLon>=900&&t.cityLon<=1900&&t.cityLat>=-150&&t.cityLat<=600;
    if(city)take(t.cityLon,t.cityLat);
    TyphoonMap map;map.fit(w0,e0,s0,n0,224,kMapBottom-kMapTop);
    const auto x=[&](int lon){return map.x(lon/10.);};const auto y=[&](int lat){return kMapTop+map.y(lat/10.);};
    // Land: a scanline fill (even-odd over every outline), only the rows of the band being drawn.
    const int first=std::max(kMapTop+1,int(p.layer->_clip_area.y1-p.area.y1)),last=std::min(kMapBottom-1,int(p.layer->_clip_area.y2-p.area.y1));
    for(int row=first;row<=last;++row){
        const int lat=int(std::lround(map.lat(row-kMapTop+0.5)*10));int cuts[96];unsigned n=0;
        for(unsigned r=0;r+1<std::size(kLandRings);++r)for(unsigned i=kLandRings[r],end=kLandRings[r+1];i<end;++i){
            const auto& a=kLandPoints[i];const auto& b=kLandPoints[i+1<end?i+1:kLandRings[r]];
            if((a[1]>lat)==(b[1]>lat)||n==std::size(cuts))continue;
            cuts[n++]=x(a[0]+(lat-a[1])*(b[0]-a[0])/(b[1]-a[1]));
        }
        std::sort(cuts,cuts+n);
        for(unsigned i=0;i+1<n;i+=2){const int from=std::max(cuts[i],1),to=std::min(cuts[i+1],222);if(from<=to)p.rect(from,row,to,row,kLand);}
    }
    // The grid, labelled on the left and bottom edges.
    const int step=(e0-w0>25||n0-s0>25)?10:5;
    for(int lon=int(std::ceil(map.west/step))*step;;lon+=step){const int at=x(lon*10);if(at>220)break;if(at<4)continue;
        p.rect(at,kMapTop+1,at,kMapBottom-1,kGrid);snprintf(text,sizeof(text),"%dE",lon);p.text(text,at+2,kMapBottom-16,at+40,0x5f7f94,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);}
    for(int lat=int(std::floor(map.north/step))*step;;lat-=step){const int at=y(lat*10);if(at>kMapBottom-20)break;if(at<kMapTop+4)continue;
        p.rect(1,at,222,at,kGrid);snprintf(text,sizeof(text),"%dN",lat);p.text(text,4,at-15,44,0x5f7f94,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);}
    if(city){const int cx=x(t.cityLon),cy=y(t.cityLat);p.rect(cx-3,cy-3,cx+3,cy+3,kAccent,1);p.text(t.city,cx+6,cy-10,cx+80,kAccent);}
    // The track coloured by grade, then the forecast dashed from where it is now.
    for(unsigned i=1;i<t.points;++i)p.line(x(t.track[i-1].lon),y(t.track[i-1].lat),x(t.track[i].lon),y(t.track[i].lat),kTyphoonColors[std::min<unsigned>(t.track[i].grade,6)],2);
    int fx=x(t.track[t.points-1].lon),fy=y(t.track[t.points-1].lat);
    for(unsigned i=0;i<ahead;++i){
        const int tx=x(t.forecast[i].lon),ty=y(t.forecast[i].lat);const double length=std::hypot(tx-fx,ty-fy);
        for(double d=0;d<length;d+=8){const double e=std::min(length,d+4);p.line(fx+int((tx-fx)*d/length),fy+int((ty-fy)*d/length),fx+int((tx-fx)*e/length),fy+int((ty-fy)*e/length),0xd8e6f0,1);}
        p.rect(tx-2,ty-2,tx+2,ty+2,kTyphoonColors[std::min<unsigned>(t.forecast[i].grade,6)],2);fx=tx;fy=ty;
    }
    const auto& now=t.track[t.points-1];const int nx=x(now.lon),ny=y(now.lat);
    p.rect(nx-7,ny-7,nx+7,ny+7,0xffffff,7);p.rect(nx-5,ny-5,nx+5,ny+5,kTyphoonColors[std::min<unsigned>(now.grade,6)],5);
}
void drawTyphoon(lv_event_t* e){
    const auto& m=*drawModel;const auto& t=m.page.typhoon;auto p=painter(e);char text[80],when[16];
    if(!t.count){p.text(m.livePhase==LivePhase::Error?m.liveState:"正在获取台风",0,100,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    const unsigned at=m.typhoonIndex%t.count;const auto& row=t.rows[at];
    snprintf(text,sizeof(text),"%s%s%s",row.number,*row.number?"  ":"",row.name);p.text(text,4,0,150,kText);
    p.rect(136,1,190,19,row.active?kAccent:kPanel,9);p.text(row.active?"活跃":"已停编",136,1,190,row.active?kBg:kMuted,LV_TEXT_ALIGN_CENTER);
    snprintf(text,sizeof(text),"%u/%u",at+1,t.count);p.text(text,192,3,222,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    p.rect(0,kMapTop,223,kMapBottom,kPanel,8);
    if(t.shown!=at||!t.points){p.text(m.livePhase==LivePhase::Error?m.liveState:"正在加载路径…",0,(kMapTop+kMapBottom)/2-10,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    drawTyphoonMap(p,t,row.active);
    const auto& now=t.track[t.points-1];constexpr int line=kMapBottom+6;
    if(row.active){
        const uint32_t color=kTyphoonColors[std::min<unsigned>(now.grade,6)];
        p.rect(0,line,92,line+21,color,6);p.text(kTyphoonGrades[std::min<unsigned>(now.grade,6)],0,line+1,92,kBg,LV_TEXT_ALIGN_CENTER);
        force(text,sizeof(text),t.wind);p.text(text,96,line+1,222,kText,LV_TEXT_ALIGN_RIGHT);
        if(t.cityLat!=INT16_MIN)snprintf(text,sizeof(text),"距%s %.0f km",t.city,distanceKm(t.cityLat/10.,t.cityLon/10.,now.lat/10.,now.lon/10.));else snprintf(text,sizeof(text),"中心气压");
        p.text(text,0,line+26,160,kMuted);snprintf(text,sizeof(text),"%u hPa",t.pressure);p.text(text,160,line+26,222,kText,LV_TEXT_ALIGN_RIGHT);
        const char* heading=typhoonHeading(t.heading);
        if(*heading&&t.speed)snprintf(text,sizeof(text),"向%s %u km/h",heading,t.speed);else snprintf(text,sizeof(text),"移动缓慢");
        p.text(text,0,line+50,130,kMuted);fixTime(t.at,when,sizeof(when));p.text(when,130,line+50,222,kMuted,LV_TEXT_ALIGN_RIGHT);
    }else{
        const uint32_t color=kTyphoonColors[std::min<unsigned>(t.peakGrade,6)];
        snprintf(text,sizeof(text),"最强 %s",kTyphoonGrades[std::min<unsigned>(t.peakGrade,6)]);p.text(text,0,line+1,130,color);
        snprintf(text,sizeof(text),"%u hPa",t.lowest);p.text(text,130,line+1,222,kText,LV_TEXT_ALIGN_RIGHT);
        p.text("最大风速",0,line+26,80,kMuted);force(text,sizeof(text),t.peakWind);p.text(text,80,line+26,222,kText,LV_TEXT_ALIGN_RIGHT);
        p.text("最后定位",0,line+50,120,kMuted);fixTime(t.at,when,sizeof(when));p.text(when,120,line+50,222,kMuted,LV_TEXT_ALIGN_RIGHT);
    }
}
void initTyphoon(lv_obj_t* screen){
    tyRoot=lv_obj_create(screen);box(tyRoot,0,35,240,257,kBg);
    tyView=lv_obj_create(tyRoot);box(tyView,8,2,224,255,kBg);lv_obj_add_event_cb(tyView,drawTyphoon,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderTyphoon(const Model& m){drawModel=&m;lv_obj_invalidate(tyView);}
// 百度热榜: eight rows at a time (rank, title, Baidu's 新/热 tag, heat), the picked one highlighted; the detail shows
// its full title and Baidu's summary.
lv_obj_t *hotRoot,*hotView;
uint32_t hotRankColor(unsigned rank){return rank==1?0xf04848:rank==2?0xff8a3c:rank==3?0xf5b83d:kMuted;}
void hotHeat(char* out,size_t size,uint32_t score){if(score>=10000)snprintf(out,size,"%lu万",static_cast<unsigned long>((score+5000)/10000));else snprintf(out,size,"%lu",static_cast<unsigned long>(score));}
// Ranks count from 1 under a pinned item, which shows 顶.
unsigned hotRank(const Model& m,unsigned i){return i+(m.hotCount&&m.page.hot.rows[0].tag==kHotPinned?0:1);}
void hotTag(Painter& p,uint8_t tag,int x,int y){
    if(tag!=1&&tag!=3)return;
    p.rect(x,y,x+19,y+19,tag==1?0xff8a3c:0xf04848,5);p.text(tag==1?"新":"热",x,y+1,x+19,0xffffff,LV_TEXT_ALIGN_CENTER);
}
void drawHotList(Painter& p,const Model& m){
    char text[64],heat[16];
    if(m.livePhase==LivePhase::Error)snprintf(text,sizeof(text),"%s",m.liveState);
    else if(m.hotUpdated&&m.clockValid){const time_t at=m.hotUpdated;tm t{};localtime_r(&at,&t);snprintf(text,sizeof(text),"百度热搜  ·  %02d:%02d 更新",t.tm_hour,t.tm_min);}
    else snprintf(text,sizeof(text),"百度热搜");
    p.text(text,4,0,180,kMuted);
    snprintf(text,sizeof(text),"%u/%u",m.hotIndex+1,m.hotCount);p.text(text,180,3,219,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    const unsigned first=m.hotIndex>7?m.hotIndex-7:0;
    for(unsigned i=first;i<m.hotCount&&i<first+8;++i){
        const auto& r=m.page.hot.rows[i];const int y=24+(i-first)*29;const unsigned rank=hotRank(m,i);
        p.rect(0,y,223,y+25,i==m.hotIndex?kBorder:kPanel,6);
        if(r.tag==kHotPinned)p.text("顶",2,y+3,28,0xf04848,LV_TEXT_ALIGN_CENTER);
        else{snprintf(text,sizeof(text),"%u",rank);p.text(text,2,y+5,28,hotRankColor(rank),LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);}
        hotHeat(heat,sizeof(heat),r.score);
        const bool tagged=r.tag==1||r.tag==3;const int right=tagged?144:166;
        const char* end=lineEnd(r.title,right-32);fitText(text,sizeof(text),r.title,end-r.title);p.text(text,32,y+3,right,kText);
        if(tagged)hotTag(p,r.tag,148,y+3);
        p.text(heat,170,y+3,219,kMuted,LV_TEXT_ALIGN_RIGHT);
    }
}
void drawHotDetail(Painter& p,const Model& m){
    char text[64],heat[16];const auto& h=m.page.hot;const auto& r=h.rows[m.hotIndex%m.hotCount];const unsigned rank=hotRank(m,m.hotIndex);
    const uint32_t color=r.tag==kHotPinned?0xf04848:hotRankColor(rank)==kMuted?kBorder:hotRankColor(rank);
    p.rect(0,0,34,21,color,6);
    if(r.tag==kHotPinned)p.text("置顶",0,1,34,0xffffff,LV_TEXT_ALIGN_CENTER);else{snprintf(text,sizeof(text),"%u",rank);p.text(text,0,3,34,0xffffff,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);}
    hotHeat(heat,sizeof(heat),r.score);snprintf(text,sizeof(text),"热度 %s",heat);p.text(text,42,1,150,kMuted);
    hotTag(p,r.tag,150,1);
    snprintf(text,sizeof(text),"%u/%u",m.hotIndex+1,m.hotCount);p.text(text,180,3,219,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    const bool ready=h.detailFor==m.hotIndex;
    paragraph(p,ready?h.title:r.title,30,2,kText);
    p.rect(0,74,223,74,kBorder);
    if(!ready)p.text(m.livePhase==LivePhase::Error?m.liveState:"正在加载…",0,84,223,kMuted);
    else if(!*h.desc)p.text("暂无摘要",0,84,223,kMuted);
    else paragraph(p,h.desc,82,7,kMuted);
    if(m.hotUpdated&&m.clockValid){const time_t at=m.hotUpdated;tm t{};localtime_r(&at,&t);snprintf(text,sizeof(text),"百度热搜  ·  %02d:%02d 更新",t.tm_hour,t.tm_min);p.text(text,0,232,223,0x5f7f94);}
}
void drawHot(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);
    if(!m.hotCount){p.text(m.livePhase==LivePhase::Error?m.liveState:"正在获取百度热搜",0,100,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    if(m.hotDetail)drawHotDetail(p,m);else drawHotList(p,m);
}
void initHot(lv_obj_t* screen){
    hotRoot=lv_obj_create(screen);box(hotRoot,0,35,240,257,kBg);
    hotView=lv_obj_create(hotRoot);box(hotView,8,2,224,255,kBg);lv_obj_add_event_cb(hotView,drawHot,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderHot(const Model& m){drawModel=&m;lv_obj_invalidate(hotView);}
// 语音笔记: a 录音 row over the notes (their start, time and length), the recording screen, or one note to read.
lv_obj_t *memoRoot,*memoView;
constexpr uint32_t kMemoRed=0xff5a5a;
void memoTime(char* out,size_t size,uint32_t at,bool year){
    if(!at){snprintf(out,size,"时间未知");return;}
    const time_t t=at;tm d{};localtime_r(&t,&d);
    if(year)snprintf(out,size,"%04d-%02d-%02d %02d:%02d",d.tm_year+1900,d.tm_mon+1,d.tm_mday,d.tm_hour,d.tm_min);
    else snprintf(out,size,"%02d/%02d %02d:%02d",d.tm_mon+1,d.tm_mday,d.tm_hour,d.tm_min);
}
// The cloud link at the top right: ready, connecting, idle (reconnects on Confirm) or failed.
void memoLink(Painter& p,const Model& m){
    const bool idle=!strcmp(m.liveState,"云端待命");
    const auto [label,color]=m.livePhase==LivePhase::Error?std::pair{"未连接",kMemoRed}:m.livePhase==LivePhase::Loading?std::pair{"连接中",kWarn}:idle?std::pair{"待命",kMuted}:std::pair{"云端就绪",kAccent};
    p.rect(146,6,153,13,color,4);p.text(label,158,0,223,color);
}
void drawMemoList(Painter& p,const Model& m,uint64_t now){
    char text[96];
    if(m.memoArmed){p.rect(0,0,223,21,kMemoRed,6);p.text("再长按下键删除选中的笔记",0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)p.text(m.lastNotice,0,0,150,kWarn);
    else if(m.livePhase==LivePhase::Error)p.text(m.liveState,0,0,140,kMemoRed);
    else if(m.memoSavedAt&&now-m.memoSavedAt<4)p.text("已保存为文字",0,0,140,kAccent);
    else{if(m.memoIndex)snprintf(text,sizeof(text),"第 %u / %u 条",m.memoIndex,m.memoCount);else snprintf(text,sizeof(text),"共 %u 条笔记",m.memoCount);p.text(text,0,0,140,kMuted);}
    if(!*m.lastNotice&&!m.memoArmed)memoLink(p,m);
    // The 录音 row.
    p.rect(0,24,223,67,m.memoIndex?kPanel:kBorder,8);
    p.rect(8,30,39,61,m.memoIndex?kBorder:kMemoRed,16);p.text(ICON_MIC,8,35,39,0xffffff,LV_TEXT_ALIGN_CENTER,&passport_icon_font_20);
    p.text("按确认开始录音",50,25,220,kText);p.text("只保存文字，不留录音",50,45,220,kMuted);
    if(!m.memoCount){p.text("还没有笔记",0,120,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("说完再按一次确认，识别的文字存在胸牌上",0,146,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);return;}
    for(unsigned i=0;i<Model::kMemoRows&&m.memoTop+i<m.memoCount;++i){
        const auto& r=m.page.memo.rows[i];const int y=74+int(i)*45;const bool picked=m.memoIndex==m.memoTop+i+1;
        p.rect(0,y,223,y+41,picked?kBorder:kPanel,8);
        // The first line; one cut short (or a note longer than its head) ends in "…", which needs its own room.
        const char* end=lineEnd(r.head,210);unsigned shown=0;for(const char* c=r.head;c<end;++c)shown+=(static_cast<unsigned char>(*c)&0xc0)!=0x80;
        const bool more=*end||r.chars>shown;if(more)end=lineEnd(r.head,210-lv_font_get_glyph_width(LV_FONT_DEFAULT,0x2026,0));
        fitText(text,sizeof(text)-3,r.head,end-r.head);if(more)strcat(text,"…");
        p.text(text,8,y+2,219,kText);
        memoTime(text,sizeof(text),r.at,false);p.text(text,8,y+22,150,kMuted,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_14);
        snprintf(text,sizeof(text),"%u 字",r.chars);p.text(text,150,y+20,215,kMuted,LV_TEXT_ALIGN_RIGHT);
    }
}
// The listening screen of 语音笔记 and 语音输入, then their recognising screen; note: the line under it for each.
void drawListening(Painter& p,const Model& m,uint64_t now,const char* listenNote,const char* recognizeNote,const char* listenTitle){
    const bool listening=m.listening();char text[32];
    const unsigned seconds=listening&&m.listenAt&&now>m.listenAt?std::min<unsigned>(now-m.listenAt,60):0;
    // A ring that breathes once a second while listening.
    const int r=listening&&seconds%2?60:56;
    p.rect(112-r,96-r,112+r,96+r,listening?0x3a1418:kPanel,r);
    p.rect(112-44,96-44,112+44,96+44,listening?kMemoRed:kBorder,44);
    p.text(ICON_MIC,62,84,162,0xffffff,LV_TEXT_ALIGN_CENTER,&passport_icon_font_24);
    if(listening){snprintf(text,sizeof(text),"%02u:%02u",seconds/60,seconds%60);p.text(text,0,166,223,kText,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_24);}
    p.text(listening?listenTitle:"正在识别…",0,200,223,listening?kText:kMuted,LV_TEXT_ALIGN_CENTER);
    p.text(listening?listenNote:recognizeNote,0,226,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
}
void drawMemoNote(Painter& p,const Model& m){
    const auto& n=m.page.memo.note;char text[100];
    if(m.memoArmed){p.rect(0,0,223,21,kMemoRed,6);p.text("再长按下键删除这条笔记",0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else{memoTime(text,sizeof(text),n.at,true);p.text(text,0,0,150,kMuted);snprintf(text,sizeof(text),"%u/%u",m.memoIndex,m.memoCount);p.text(text,150,3,219,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);}
    p.rect(0,26,223,26,kBorder);
    const char* s=n.text;unsigned line=0,shown=0;
    for(;*s;++line){
        const char* start=s;s=lineEnd(s,224);
        if(line<m.memoScroll||shown==Model::kMemoLines)continue;
        fitText(text,sizeof(text),start,s-start);p.text(text,0,32+int(shown++)*20,223,kText);
    }
    if(line>Model::kMemoLines){snprintf(text,sizeof(text),"%u-%u / %u 行",m.memoScroll+1,m.memoScroll+shown,line);p.text(text,0,234,223,0x5f7f94,LV_TEXT_ALIGN_RIGHT);}
}
void drawMemo(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;
    if(m.voiceBusy())drawListening(p,m,now,"最长 60 秒 · 只保存文字","识别完就断开，不接收回答");else if(m.memoDetail)drawMemoNote(p,m);else drawMemoList(p,m,now);
}
void initMemo(lv_obj_t* screen){
    memoRoot=lv_obj_create(screen);box(memoRoot,0,35,240,257,kBg);
    memoView=lv_obj_create(memoRoot);box(memoView,8,2,224,255,kBg);lv_obj_add_event_cb(memoView,drawMemo,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderMemo(const Model& m){drawModel=&m;lv_obj_invalidate(memoView);}
// 语音输入: the computer's assistant has XiaoZhi recognise the speech and pastes the text where the cursor is.
// Ready: a big button over the last input; then the listening and recognising screens of 语音笔记.
lv_obj_t *dictationRoot,*dictationView;
constexpr uint32_t kDictation=0xb59cff;
void drawDictation(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;
    if(m.voiceBusy()){drawListening(p,m,now,"最长 60 秒 · 到时自动结束","文字将输入到电脑光标处");return;}
    const bool done=!strcmp(m.liveValue,"done"),failed=!strcmp(m.liveValue,"empty")||!strcmp(m.liveValue,"fail");
    // Status: the last outcome (a success fades after 4 s, a failure stays), then the link on the right.
    if(failed||!m.bridgeAlive)p.text(failed?m.liveState:"未连接电脑助手",0,0,223,kMemoRed);
    else if(!strcmp(m.liveValue,"key")&&now-m.dictationAt<4)p.text(m.liveState,0,0,150,kAccent);
    else p.text(done&&now-m.dictationAt<4?"已输入到电脑":"输入到电脑光标处",0,0,150,done&&now-m.dictationAt<4?kAccent:kMuted);
    if(!failed&&m.bridgeAlive&&*m.liveLines[2]){p.rect(170,6,177,13,kAccent,4);p.text(m.liveLines[2],182,0,223,kAccent);}
    p.rect(112-46,76-46,112+46,76+46,0x2a2440,46);
    p.rect(112-38,76-38,112+38,76+38,kDictation,38);
    p.text(ICON_MIC,62,64,162,kBg,LV_TEXT_ALIGN_CENTER,&passport_icon_font_24);
    p.text("按确认开始说话",0,132,223,kText,LV_TEXT_ALIGN_CENTER);
    p.text("下键：回车    上键：删除整行",0,154,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
    // The last input: its start (two lines, "…" when cut short), length and time.
    p.rect(0,182,223,254,kPanel,8);
    if(!*m.liveLines[0]){p.text("还没有输入",0,206,223,kMuted,LV_TEXT_ALIGN_CENTER);return;}
    p.text("上次输入",8,186,100,kMuted);p.text(m.liveLines[1],100,186,215,kMuted,LV_TEXT_ALIGN_RIGHT);
    char text[96];const char* s=m.liveLines[0];
    for(int line=0;line<2&&*s;++line){
        const char* end=lineEnd(s,207);
        if(line==1&&*end)end=lineEnd(s,207-lv_font_get_glyph_width(LV_FONT_DEFAULT,0x2026,0));
        fitText(text,sizeof(text)-3,s,end-s);if(line==1&&*end)strcat(text,"…");
        p.text(text,8,208+line*21,215,kText);s=end;
    }
}
void initDictation(lv_obj_t* screen){
    dictationRoot=lv_obj_create(screen);box(dictationRoot,0,35,240,257,kBg);
    dictationView=lv_obj_create(dictationRoot);box(dictationView,8,2,224,255,kBg);lv_obj_add_event_cb(dictationView,drawDictation,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderDictation(const Model& m){drawModel=&m;lv_obj_invalidate(dictationView);}
lv_obj_t* voiceLayer(lv_obj_t* parent,const char* const* notes){
    auto* layer=lv_obj_create(parent);box(layer,8,2,224,255,kBg);visible(layer,false);
    lv_obj_add_event_cb(layer,[](lv_event_t* e){auto p=painter(e);const auto* notes=static_cast<const char* const*>(lv_event_get_user_data(e));
        drawListening(p,*drawModel,esp_timer_get_time()/1000000,notes[0],notes[1]);},LV_EVENT_DRAW_MAIN,const_cast<char**>(notes));
    return layer;
}
void initStatusVoice(lv_obj_t* screen){
    statusVoice=lv_obj_create(screen);box(statusVoice,8,37,224,255,kBg);visible(statusVoice,false);
    lv_obj_add_event_cb(statusVoice,[](lv_event_t* e){auto p=painter(e);drawListening(p,*drawModel,esp_timer_get_time()/1000000,"一句话，12 个字以内","识别后直接显示在状态牌上");},LV_EVENT_DRAW_MAIN,nullptr);
}
// 万年历's third view: the day's 月相 (moon.hpp, computed on the badge) over its solar terms.
struct MoonDay{int day{INT32_MIN};double elongation,age;int today;double next[4];};
// The draw callback runs once per strip of the screen; the searches (about 50 ms of soft floats) run once per day.
const MoonDay& moonDay(const Model& m){
    static MoonDay cache;
    const int day=dayNumber({m.calendarYear,m.calendarMonth,m.calendarDay});
    if(cache.day==day)return cache;
    tm t{};t.tm_year=m.calendarYear-1900;t.tm_mon=m.calendarMonth-1;t.tm_mday=m.calendarDay;t.tm_isdst=-1;
    const double start=static_cast<double>(mktime(&t)),noon=start+43200;
    cache={day,moonElongation(noon),0,-1,{}};
    for(int q=0;q<4;++q){cache.next[q]=moonPhaseAfter(start,q);if(cache.next[q]<start+86400)cache.today=q;}
    // The age counts from the last new moon before noon.
    double newMoon=moonPhaseAfter(noon-31*86400.0,0);
    for(double later;(later=moonPhaseAfter(newMoon+3600,0))<=noon;)newMoon=later;
    cache.age=(noon-newMoon)/86400;
    return cache;
}
constexpr uint32_t kMoonLight=0xf3e3b0;
const char* const kSolarTermNotes[24]{"进入一年最冷的时段","一年中最冷的时候","春季开始，万物复苏","降雨增多，冰雪消融","春雷始鸣，惊醒蛰虫","昼夜等长，此后昼长",
    "天气清明，踏青扫墓","雨生百谷，春季将尽","夏季开始，气温升高","麦粒渐满，尚未成熟","麦熟抢收，农忙时节","一年中白天最长",
    "天气炎热，尚未最热","一年中最热的时候","秋季开始，暑热未尽","暑气结束，早晚渐凉","夜间转凉，草木凝露","昼夜等长，此后夜长",
    "露水更凉，深秋渐寒","天气渐冷，开始有霜","冬季开始，注意保暖","开始降雪，雪量不大","降雪增多，可能积雪","一年中白天最短"};
void drawCalMoon(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const auto& d=moonDay(m);char text[64];
    static const char* const week[]{"日","一","二","三","四","五","六"};
    const Date date{m.calendarYear,m.calendarMonth,m.calendarDay};const int today=dayNumber(date);
    snprintf(text,sizeof(text),"%d月%d日 星期%s",date.month,date.day,week[weekday(date)]);p.text(text,0,0,150,kText);
    LunarDate lunar{};toLunar(date,lunar);snprintf(text,sizeof(text),"农历%s%s%s",lunar.leap?"闰":"",lunarMonthName(lunar.month),lunarDayName(lunar.day));p.text(text,100,0,223,kAccent,LV_TEXT_ALIGN_RIGHT);
    // The disc: the dark side, then the lit part row by row (waxing lights the right, waning the left).
    constexpr int cx=50,cy=74,r=46;
    p.rect(cx-r,cy-r,cx+r,cy+r,0x1c2b38,r);
    const double c=cos(d.elongation*3.14159265358979323846/180);const bool waxing=d.elongation<180;
    const int top=std::max(cy-r,int(p.layer->_clip_area.y1-p.area.y1)),bottom=std::min(cy+r,int(p.layer->_clip_area.y2-p.area.y1));
    for(int y=top;y<=bottom;++y){
        const double dy=y-cy+0.5,w=sqrt(std::max(0.0,r*r-dy*dy))-0.4;
        const int x1=int(lround(cx+(waxing?w*c:-w))),x2=int(lround(cx+(waxing?w:-w*c)))-1;
        if(w>0&&x2>=x1)p.rect(x1,y,x2,y,kMoonLight);
    }
    // Beside it: the phase, the lit share, the age, and the next full or new moon.
    p.text(moonPhaseName(d.today,d.elongation),106,22,223,kMoonLight,LV_TEXT_ALIGN_LEFT,&passport_ui_font_x2);
    snprintf(text,sizeof(text),"照亮 %d%%",int(lround(moonLit(d.elongation)*100)));p.text(text,106,60,223,kText);
    snprintf(text,sizeof(text),"月龄 %.1f 天",d.age);p.text(text,106,81,223,kMuted);
    const auto localDay=[](double at){const time_t t=static_cast<time_t>(at);tm l{};localtime_r(&t,&l);return dayNumber({l.tm_year+1900,l.tm_mon+1,l.tm_mday});};
    const int major=d.next[2]<d.next[0]?2:0,until=localDay(d.next[major])-today;
    if(until>0)snprintf(text,sizeof(text),"%s还有 %d 天",major?"满月":"新月",until);else snprintf(text,sizeof(text),"今天%s",major?"满月":"新月");
    p.text(text,106,102,223,kAccent);
    // The next four quarters, soonest first.
    p.rect(0,126,223,204,kPanel,8);
    int order[4]{0,1,2,3};std::sort(order,order+4,[&](int a,int b){return d.next[a]<d.next[b];});
    for(int i=0;i<4;++i){
        const int q=order[i],y=129+i*19;const time_t at=static_cast<time_t>(d.next[q]);tm t{};localtime_r(&at,&t);
        p.text(moonQuarterName(q),8,y,60,q==2?kMoonLight:kText);
        snprintf(text,sizeof(text),"%d月%d日 周%s",t.tm_mon+1,t.tm_mday,week[t.tm_wday]);p.text(text,64,y,170,kMuted);
        snprintf(text,sizeof(text),"%02d:%02d",t.tm_hour,t.tm_min);p.text(text,160,y+2,215,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    }
    // The solar term in force and the next one.
    int last,next;Date lastDate{},nextDate{};
    if(!solarTermsAround(date,last,lastDate,next,nextDate))return;
    p.rect(0,210,223,254,kPanel,8);
    snprintf(text,sizeof(text),"%s · %s",kSolarTermNames[last],kSolarTermNotes[last]);p.text(text,8,212,215,kText);
    snprintf(text,sizeof(text),"下个 %s  %d月%d日",kSolarTermNames[next],nextDate.month,nextDate.day);p.text(text,8,232,160,kMuted);
    snprintf(text,sizeof(text),"%d 天后",dayNumber(nextDate)-today);p.text(text,150,232,215,kAccent,LV_TEXT_ALIGN_RIGHT);
}
// 待办提醒: a 录音 row over the todos (the soonest reminder first, done ones last and struck through), the card of a
// reminder going off, or the listening screen.
lv_obj_t *todoRoot,*todoView;
constexpr uint32_t kTodo=0xf5a524;
void drawTodo(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;const auto& list=m.page.todo;
    const time_t clock=time(nullptr);char text[100],when[32];
    if(m.voiceBusy()){drawListening(p,m,now,"例：明天下午三点开会","说了时间就会准时提醒");return;}
    if(m.todoRinging){
        // A steady card (a flashing one tears across the screen's strips); the clock blinks.
        p.rect(0,0,223,252,kTodo,14);p.rect(3,3,220,249,kPanel,12);
        if(now%2)p.text(ICON_CLOCK,0,16,223,kTodo,LV_TEXT_ALIGN_CENTER,&passport_icon_font_24);
        p.text("待办提醒",0,48,223,kTodo,LV_TEXT_ALIGN_CENTER);
        // The todo in large type (two lines of seven), or four lines at the normal size.
        const char* s=m.todoNext;const char* first=lineEnd(s,216,&passport_ui_font_x2);
        if(!*lineEnd(first,216,&passport_ui_font_x2)){
            fitText(text,sizeof(text),s,first-s);p.text(text,4,82,219,kText,LV_TEXT_ALIGN_CENTER,&passport_ui_font_x2);
            if(*first){fitText(text,sizeof(text),first,strlen(first));p.text(text,4,124,219,kText,LV_TEXT_ALIGN_CENTER,&passport_ui_font_x2);}
        }else paragraph(p,s,84,4,kText,8,215);
        if(m.todoNextAt){todoWhen(m.todoNextAt,clock,when,sizeof(when));p.text(when,0,172,223,kMuted,LV_TEXT_ALIGN_CENTER);}
        p.text("确认：完成",0,206,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);p.text("上下：10 分钟后再提醒",0,226,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
        return;
    }
    unsigned open=0;for(unsigned i=0;i<list.count;++i)open+=!(list.items[i].flags&TodoList::kDone);
    if(m.todoArmed){p.rect(0,0,223,21,kMemoRed,6);p.text("再长按下键删除选中的待办",0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)p.text(m.lastNotice,0,0,223,kWarn);
    else{snprintf(text,sizeof(text),"待办 %u 条 · 未完成 %u",list.count,open);p.text(text,0,0,170,kMuted);
        if(list.count>Model::kTodoRows){snprintf(text,sizeof(text),"%u/%u",m.todoIndex?m.todoIndex:0,list.count);p.text(text,170,3,223,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);}}
    // The 录音 row.
    const bool rec=!m.todoIndex;
    p.rect(0,24,223,64,rec?kBorder:kPanel,8);p.rect(8,30,36,58,rec?kTodo:kBorder,14);
    p.text(ICON_MIC,8,33,36,0xffffff,LV_TEXT_ALIGN_CENTER,&passport_icon_font_20);
    p.text("按确认说一条待办",46,24,220,kText);p.text("可带时间：明天三点",46,43,220,kMuted);
    if(!list.count){p.text("还没有待办",0,120,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("说「20分钟后提醒我拿外卖」试试",0,146,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);return;}
    for(unsigned i=0;i<Model::kTodoRows&&m.todoTop+i<list.count;++i){
        const auto& t=list.items[m.todoTop+i];const int y=70+int(i)*46;const bool picked=m.todoIndex==m.todoTop+i+1,done=t.flags&TodoList::kDone;
        p.rect(0,y,223,y+42,picked?kBorder:kPanel,8);
        // The box: ticked when done.
        if(done){p.rect(6,y+10,28,y+32,0x22c55e,5);p.text(LV_SYMBOL_OK,6,y+11,28,kBg,LV_TEXT_ALIGN_CENTER,&passport_icon_font_20);}
        else{p.rect(6,y+10,28,y+32,kMuted,5);p.rect(8,y+12,26,y+30,picked?kBorder:kPanel,4);}
        const char* end=lineEnd(t.text,182);const bool more=*end;if(more)end=lineEnd(t.text,182-lv_font_get_glyph_width(LV_FONT_DEFAULT,0x2026,0));
        fitText(text,sizeof(text)-3,t.text,end-t.text);if(more)strcat(text,"…");
        p.text(text,34,y+1,219,done?0x5f7f94:kText);
        if(done){lv_point_t size{};lv_text_get_size(&size,text,LV_FONT_DEFAULT,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);p.rect(34,y+11,34+size.x,y+11,0x5f7f94);}
        // When it reminds: overdue in red, today's in amber.
        if(t.at){todoWhen(t.at,clock,when,sizeof(when));const bool late=!done&&static_cast<time_t>(t.at)<clock;
            p.text(when,34,y+21,219,done?0x5f7f94:late?kMemoRed:!strncmp(when,"今天",6)?kTodo:kMuted);}
        else p.text(done?"已完成":"不提醒",34,y+21,219,0x5f7f94);
    }
}
void initTodo(lv_obj_t* screen){
    todoRoot=lv_obj_create(screen);box(todoRoot,0,35,240,257,kBg);
    todoView=lv_obj_create(todoRoot);box(todoView,8,2,224,255,kBg);lv_obj_add_event_cb(todoView,drawTodo,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderTodo(const Model& m){drawModel=&m;lv_obj_invalidate(todoView);}
// 语音记账: today's spending (and income, and the month the computer counted) over a 录音 row and today's entries,
// newest first; or the listening screen.
lv_obj_t *ledgerRoot,*ledgerView;
constexpr uint32_t kLedger=0x34d399;
constexpr uint32_t kLedgerColors[]{0xf59e0b,0x3b82f6,0xec4899,0x14b8a6,0x8b5cf6,0xef4444,0xf97316,0x22c55e,0x64748b,kLedger};
static_assert(std::size(kLedgerColors)==kLedgerCount);
void drawLedger(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;const auto& book=m.page.ledger;
    if(m.voiceBusy()){drawListening(p,m,now,"例：午饭 35 · 打车 28 块 5","识别后自动记下金额和分类");return;}
    char text[100],amount[24];const time_t today=ledgerToday();const auto sync=ledgerStatus();
    uint8_t order[LedgerBook::kMax];const unsigned shown=book.shown(today,order);
    // The top line: the delete question, a notice, or the date and what the computer still has to take.
    if(m.ledgerArmed){p.rect(0,0,223,21,kMemoRed,6);p.text("再长按下键删除这一笔",0,1,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)p.text(m.lastNotice,0,0,223,kWarn);
    else{
        tm t{};localtime_r(&today,&t);static const char* const week[]{"周日","周一","周二","周三","周四","周五","周六"};
        snprintf(text,sizeof(text),"%d月%d日 %s",t.tm_mon+1,t.tm_mday,week[t.tm_wday]);p.text(text,0,0,130,kMuted);
        if(sync.pending){snprintf(text,sizeof(text),"%u 笔待同步",sync.pending);p.rect(124,7,130,13,kWarn,3);p.text(text,134,0,223,kWarn,LV_TEXT_ALIGN_RIGHT);}
        else if(book.count){p.rect(166,7,172,13,kLedger,3);p.text("已同步",176,0,223,kLedger,LV_TEXT_ALIGN_RIGHT);}
    }
    // Today's totals; the month from the computer (whole yuan).
    int32_t out,in;book.totals(today,out,in);
    p.rect(0,24,223,84,kPanel,8);
    p.text("今日支出",10,27,120,kMuted);
    ledgerAmount(out,amount,sizeof(amount));p.text(amount,10,50,120,kText,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_24);
    p.rect(126,32,126,76,kBorder);
    p.text("收入",134,27,180,kMuted);ledgerAmount(in,amount,sizeof(amount));*strchr(amount,'.')=0;
    p.text(amount,160,30,215,in?kLedger:kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    p.text("本月",134,55,180,kMuted);
    if(sync.monthKnown){ledgerAmount(sync.monthOut,amount,sizeof(amount));*strchr(amount,'.')=0;}else strcpy(amount,"--");
    p.text(amount,160,58,215,kText,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    // The 录音 row.
    const bool rec=!m.ledgerIndex;
    p.rect(0,90,223,128,rec?kBorder:kPanel,8);p.rect(8,95,36,123,rec?kLedger:kBorder,14);
    p.text(ICON_MIC,8,98,36,0xffffff,LV_TEXT_ALIGN_CENTER,&passport_icon_font_20);
    p.text("按确认记一笔",46,90,220,kText);p.text("说用途和金额：午饭 35",46,108,220,kMuted);
    if(!shown){p.text("今天还没有记账",0,168,223,kMuted,LV_TEXT_ALIGN_CENTER);p.text("连上电脑时自动同步到电脑",0,194,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);return;}
    for(unsigned i=0;i<Model::kLedgerRows&&m.ledgerTop+i<shown;++i){
        const auto& entry=book.items[order[m.ledgerTop+i]];const int y=134+int(i)*40;const bool picked=m.ledgerIndex==m.ledgerTop+i+1;
        const bool income=entry.category==kLedgerIncome;const uint32_t color=kLedgerColors[entry.category];
        p.rect(0,y,223,y+36,picked?kBorder:kPanel,8);
        // The category's first character on its colour.
        p.rect(6,y+6,30,y+30,color,6);fitText(text,sizeof(text),kLedgerCategories[entry.category],3);p.text(text,6,y+8,30,kBg,LV_TEXT_ALIGN_CENTER);
        const char* end=lineEnd(entry.note,104);const bool more=*end;if(more)end=lineEnd(entry.note,104-lv_font_get_glyph_width(LV_FONT_DEFAULT,0x2026,0));
        fitText(text,sizeof(text)-3,entry.note,end-entry.note);if(more)strcat(text,"…");p.text(text,38,y,146,kText);
        const time_t at=entry.at;tm t{};localtime_r(&at,&t);snprintf(text,sizeof(text),"%02d:%02d · %s",t.tm_hour,t.tm_min,kLedgerCategories[entry.category]);
        p.text(text,38,y+18,150,kMuted);
        ledgerAmount(entry.cents,amount,sizeof(amount));snprintf(text,sizeof(text),"%s%s",income?"+":"-",amount);
        p.text(text,140,y+10,215,income?kLedger:kText,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
        // Not on the computer yet.
        if(!(entry.flags&LedgerBook::kSynced))p.rect(212,y+4,216,y+8,kWarn,2);
    }
}
void initLedger(lv_obj_t* screen){
    ledgerRoot=lv_obj_create(screen);box(ledgerRoot,0,35,240,257,kBg);
    ledgerView=lv_obj_create(ledgerRoot);box(ledgerView,8,2,224,255,kBg);lv_obj_add_event_cb(ledgerView,drawLedger,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderLedger(const Model& m){drawModel=&m;lv_obj_invalidate(ledgerView);}
// 语音计算器: the picked sum and its result in the largest type that fits, then this visit's sums (newest first).
lv_obj_t *calcRoot,*calcView;
constexpr uint32_t kCalc=0xfbbf24;
void drawCalc(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;
    if(m.voiceBusy()){drawListening(p,m,now,"例：128 乘 37 · 根号 2","说完自动算出结果");return;}
    char text[100];
    // The top line: what went wrong, else which sum is on show.
    if(*m.lastNotice){const char* end=lineEnd(m.lastNotice,214);const bool more=*end;if(more)end=lineEnd(m.lastNotice,200);
        fitText(text,sizeof(text)-3,m.lastNotice,end-m.lastNotice);if(more)strcat(text,"…");p.text(text,0,0,223,kWarn);}
    else if(m.calcCount){if(m.calcIndex)snprintf(text,sizeof(text),"第 %u 条 · 共 %u 条",m.calcIndex+1u,m.calcCount);else snprintf(text,sizeof(text),"最新结果 · 共 %u 条",m.calcCount);p.text(text,0,0,223,kMuted);}
    else p.text("说出算式，按确认开始",0,0,223,kMuted);
    p.rect(0,24,223,150,kPanel,8);
    if(!m.calcCount){
        p.rect(84,38,140,94,kCalc,28);p.text(ICON_MIC,84,54,140,kBg,LV_TEXT_ALIGN_CENTER,&passport_icon_font_24);
        p.text("按确认，说出算式",0,102,223,kText,LV_TEXT_ALIGN_CENTER);p.text("乘方 · 开方 · 百分比 · 打折",0,124,223,kMuted,LV_TEXT_ALIGN_CENTER);
        static const char* const examples[]{"例：一百除以七","2 的 10 次方 · 根号 2","1000 打八五折","算完说「再乘 3」接着算"};
        for(unsigned i=0;i<std::size(examples);++i)p.text(examples[i],0,158+int(i)*24,223,i==3?kCalc:0x5f7f94,LV_TEXT_ALIGN_CENTER);
        return;
    }
    const auto& row=m.page.calc[m.calcIndex];
    // The sum as understood, on up to two lines.
    const char* s=row.shown;
    for(int line=0;line<2&&*s;++line){
        const char* end=lineEnd(s,204);const bool more=line==1&&*end;if(more)end=lineEnd(s,190);
        fitText(text,sizeof(text)-3,s,end-s);if(more)strcat(text,"…");p.text(text,10,30+line*20,214,kMuted);s=end;
    }
    const std::string value=calc::format(row.value),words=calc::reading(row.value);
    const lv_font_t* font=&lv_font_montserrat_14;
    for(const lv_font_t* f:{&lv_font_montserrat_48,&lv_font_montserrat_24,&lv_font_montserrat_20})if(!*lineEnd(value.c_str(),186,f)){font=f;break;}
    const int y=font==&lv_font_montserrat_48?72:font==&lv_font_montserrat_24?88:92;
    p.text("=",10,y+(font==&lv_font_montserrat_48?14:0),30,kCalc,LV_TEXT_ALIGN_LEFT,&lv_font_montserrat_24);
    p.text(value.c_str(),30,y,214,kText,LV_TEXT_ALIGN_RIGHT,font);
    if(!words.empty())p.text(words.c_str(),10,126,214,kCalc,LV_TEXT_ALIGN_RIGHT);
    // This visit's sums, three at a time around the picked one.
    const unsigned top=m.calcCount<=3?0u:std::min<unsigned>(m.calcIndex?m.calcIndex-1u:0u,m.calcCount-3u);
    for(unsigned i=top;i<m.calcCount&&i<top+3;++i){
        const auto& r=m.page.calc[i];const int y0=158+int(i-top)*32;const bool picked=i==m.calcIndex;
        p.rect(0,y0,223,y0+28,picked?kBorder:kPanel,6);
        // The result whole on the right; the sum gets what is left.
        const std::string v="= "+calc::format(r.value);int width=0;for(char c:v)width+=lv_font_get_glyph_width(&lv_font_montserrat_14,static_cast<unsigned char>(c),0);
        const int room=std::max(40,200-width);
        const char* end=lineEnd(r.shown,room);const bool more=*end;if(more)end=lineEnd(r.shown,room-14);
        fitText(text,sizeof(text)-3,r.shown,end-r.shown);if(more)strcat(text,"…");p.text(text,8,y0+4,8+room,picked?kText:kMuted);
        p.text(v.c_str(),12+room,y0+7,215,picked?kCalc:kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    }
}
void initCalc(lv_obj_t* screen){
    calcRoot=lv_obj_create(screen);box(calcRoot,0,35,240,257,kBg);
    calcView=lv_obj_create(calcRoot);box(calcView,8,2,224,255,kBg);lv_obj_add_event_cb(calcView,drawCalc,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderCalc(const Model& m){drawModel=&m;lv_obj_invalidate(calcView);}
// 语音指令: listening, then what was not understood with what can be said (the switched-off kinds left out).
lv_obj_t *voiceRoot,*voiceView;
void drawVoice(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);
    if(m.voiceBusy()){drawListening(p,m,esp_timer_get_time()/1000000,"例：打开 Mac 的微信","听清后马上执行","正在听，说完自动结束");return;}
    char text[100];const char* s=*m.lastNotice?m.lastNotice:"请按确认，再说一次";
    for(int line=0;line<3&&*s;++line){
        const char* end=lineEnd(s,214);const bool more=line==2&&*end;if(more)end=lineEnd(s,200);
        fitText(text,sizeof(text)-3,s,end-s);if(more)strcat(text,"…");p.text(text,0,4+line*22,223,*m.lastNotice?kWarn:kText);s=end;
    }
    p.rect(0,84,223,226,kPanel,10);p.text("可以这样说",12,92,220,kMuted);
    struct Example{const char* said;const char* does;bool on;};
    const Example examples[]{{"「打开汇率」","胸牌",m.voiceJump},{"「打开 Mac 的微信」","电脑",m.macOpen},{"「聊天」","小智",true}};
    int y=120;
    for(const auto& x:examples)if(x.on){p.text(x.said,12,y,220,kText);p.text(x.does,12,y,211,kMuted,LV_TEXT_ALIGN_RIGHT);y+=32;}
    p.text("10 秒后回到首页",0,236,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
}
void initVoice(lv_obj_t* screen){
    voiceRoot=lv_obj_create(screen);box(voiceRoot,0,35,240,257,kBg);
    voiceView=lv_obj_create(voiceRoot);box(voiceView,8,2,224,255,kBg);lv_obj_add_event_cb(voiceView,drawVoice,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderVoice(const Model& m){drawModel=&m;lv_obj_invalidate(voiceView);}
// 离座锁屏: the computer's verdict in large type, the badge's signal against the line, then the four settings.
lv_obj_t *guardRoot,*guardView;
void drawGuard(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const auto& g=m.guard;char text[64];
    // The verdict; before the computer answers, how the link is coming along.
    const bool linked=m.livePhase==LivePhase::Ready;const unsigned phase=linked?m.guardPhase:0;
    static const char* const names[]{"未连接电脑","USB 连接中","在座","即将锁屏","已锁屏"};
    static const uint32_t colors[]{kMuted,0x5fd3ff,0x22c55e,kWarn,kMemoRed};
    p.rect(0,0,223,84,kPanel,10);p.rect(0,0,5,84,colors[phase],3);
    if(phase==3)snprintf(text,sizeof(text),"%s %u",names[3],m.guardLeft);else snprintf(text,sizeof(text),"%s",names[phase]);
    p.text(text,14,4,215,colors[phase],LV_TEXT_ALIGN_LEFT,&passport_ui_font_x2);
    static const char* const keeps[]{"防锁屏已关","防锁屏 · 保持亮屏","防锁屏 · 保持在线"};
    const char* note=!linked?(m.livePhase==LivePhase::Error?m.liveState:m.connectionState):phase==1?"拔线后用蓝牙守护":phase==2?keeps[g.keep]
        :phase==3?"走远了，回来就不锁":phase==4?(g.wake?"回来时点亮屏幕":"回来后自行解锁"):"等待电脑回应";
    p.text(*m.lastNotice?m.lastNotice:note,14,52,215,*m.lastNotice?kWarn:kMuted);
    // The signal (-100 to -40 dBm) and the line under which you count as away.
    const auto x=[](int dbm){return 46+std::clamp(dbm+100,0,60)*170/60;};
    p.text("信号",0,94,44,kMuted);p.rect(46,100,216,110,kBorder,5);
    if(linked&&m.guardRssi){p.rect(46,100,x(m.guardRssi),110,m.guardRssi>=m.guardLine?0x22c55e:kWarn,5);}
    if(linked&&m.guardLine){p.rect(x(m.guardLine)-1,95,x(m.guardLine)+1,115,kText);}
    if(linked&&m.guardRssi)snprintf(text,sizeof(text),"%d dBm · 线 %d",m.guardRssi,m.guardLine);
    else snprintf(text,sizeof(text),"%s",phase==1?"USB 时不测信号":"暂无信号读数");
    p.text(text,46,112,223,0x5f7f94);
    // The settings: Up/Down pick a row, Confirm steps its value.
    static const char* const rows[]{"离开距离","离开确认","防锁屏","回来亮屏"};
    static const char* const distances[]{"近","中","远"},*const keepNames[]{"关","保持亮屏","保持在线"};
    for(int i=0;i<4;++i){
        const int y=136+i*30;const bool on=m.guardRow==i;
        p.rect(0,y,223,y+26,on?kAccent:kPanel,8);
        p.text(rows[i],10,y+3,120,on?kBg:kText);
        if(i==0)snprintf(text,sizeof(text),"%s",distances[g.distance]);
        else if(i==1)snprintf(text,sizeof(text),"%u 秒",GuardPrefs::kDelays[g.delay]);
        else if(i==2)snprintf(text,sizeof(text),"%s",keepNames[g.keep]);
        else snprintf(text,sizeof(text),"%s",g.wake?"开":"关");
        p.text(text,110,y+3,213,on?kBg:kMuted,LV_TEXT_ALIGN_RIGHT);
    }
}
void initGuard(lv_obj_t* screen){
    guardRoot=lv_obj_create(screen);box(guardRoot,0,35,240,257,kBg);
    guardView=lv_obj_create(guardRoot);box(guardView,8,2,224,255,kBg);lv_obj_add_event_cb(guardView,drawGuard,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderGuard(const Model& m){drawModel=&m;lv_obj_invalidate(guardView);}
// 中英翻译: what was said over its translation; Confirm speaks the next phrase, a long Up reads the translation aloud.
lv_obj_t *translateRoot,*translateView;
constexpr uint32_t kTranslate=0x60a5fa;
void drawTranslate(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;const auto& t=m.page.translation;char text[100];
    if(m.voiceBusy()){drawListening(p,m,now,"说中文或英文 · 最长 30 秒","识别后自动翻译");return;}
    const bool translating=!strcmp(m.liveState,"正在翻译"),reading=!strcmp(m.liveState,"正在朗读");
    // Top line: the direction (or what is happening), then the cloud link.
    if(m.livePhase==LivePhase::Error)p.text(m.liveState,0,0,140,kMemoRed);
    else if(translating||reading)p.text(translating?"正在翻译…":"正在朗读…",0,0,140,kAccent);
    else p.text(!t.at?"自动识别中英文":t.toEnglish?"中文 → 英文":"英文 → 中文",0,0,140,kMuted);
    memoLink(p,m);
    if(!t.at){
        p.rect(112-46,104-46,112+46,104+46,0x1d2c44,46);p.rect(112-38,104-38,112+38,104+38,kTranslate,38);
        p.text(ICON_MIC,62,92,162,kBg,LV_TEXT_ALIGN_CENTER,&passport_icon_font_24);
        p.text("按确认说中文或英文",0,164,223,kText,LV_TEXT_ALIGN_CENTER);
        p.text("说完再按确认，自动翻译",0,188,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
        p.text("长按上键朗读译文",0,212,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);
        return;
    }
    // What was said: two lines at most.
    p.rect(0,24,223,72,kPanel,8);paragraph(p,*t.source?t.source:"（没有听清）",26,2,kMuted,8,215,true);
    if(!t.engine){p.text(*t.source?"翻译失败，按确认再说一次":"没有听清，按确认再说一次",0,96,223,kMemoRed,LV_TEXT_ALIGN_CENTER);return;}
    // The translation, scrolled by Up/Down.
    const char* s=t.result;unsigned line=0,shown=0;
    for(;;++line){
        while(*s==' ')++s;
        if(!*s)break;
        const char* start=s;s=wrapEnd(s,224);
        if(line<m.translateScroll||shown==Model::kTranslateLines)continue;
        fitText(text,sizeof(text),start,s-start);p.text(text,0,80+int(shown++)*22,223,kText);
    }
    p.rect(0,194,223,194,kBorder);
    if(line>Model::kTranslateLines){snprintf(text,sizeof(text),"%u-%u / %u 行",m.translateScroll+1,m.translateScroll+shown,line);p.text(text,0,200,223,0x5f7f94,LV_TEXT_ALIGN_RIGHT);}
    p.text(t.engine==1?"腾讯翻译君":"MyMemory",0,200,150,0x5f7f94);
    p.text(reading?"正在朗读…":!strcmp(m.liveState,"朗读失败，长按上键重试")?"朗读失败，长按上键重试":"长按上键朗读 · 按确认继续说",0,226,223,reading?kAccent:kMuted,LV_TEXT_ALIGN_CENTER);
}
void initTranslate(lv_obj_t* screen){
    translateRoot=lv_obj_create(screen);box(translateRoot,0,35,240,257,kBg);
    translateView=lv_obj_create(translateRoot);box(translateView,8,2,224,255,kBg);lv_obj_add_event_cb(translateView,drawTranslate,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderTranslate(const Model& m){drawModel=&m;lv_obj_invalidate(translateView);}
// 随机转盘: the mode's options as a wheel; a spin runs a highlight round it that slows to the pick, shown below.
lv_obj_t *wheelRoot,*wheelView;
constexpr uint32_t kWheelColors[]{0xff6b6b,0xffa94d,0xffd43b,0x69db7c,0x38d9a9,0x4dabf7,0x9775fa,0xf783ac};
// A sector's colour mixed into the background: the options not lit while it spins.
uint32_t dim(uint32_t c){const auto mix=[&](int shift){return (((c>>shift&255)*2+(kBg>>shift&255)*3)/5)<<shift;};return mix(16)|mix(8)|mix(0);}
void drawWheel(lv_event_t* e){
    const auto& m=*drawModel;auto p=painter(e);const uint64_t now=esp_timer_get_time()/1000000;const auto& w=m.page.wheel;char text[64];
    if(m.voiceBusy()){drawListening(p,m,now,"例：吃饭有烧烤、汉堡、炸鸡","只说一项就加进当前模式");return;}
    if(!w.count){
        p.text("还没有转盘",0,70,223,kText,LV_TEXT_ALIGN_CENTER);
        p.text("长按上键说：吃饭有面条、米饭",0,100,223,kMuted,LV_TEXT_ALIGN_CENTER);
        p.text("或在手机配置页「转盘」里添加",0,124,223,kMuted,LV_TEXT_ALIGN_CENTER);
        if(*m.lastNotice)paragraph(p,m.lastNotice,212,2,kWarn);
        return;
    }
    const auto& mode=w.modes[w.current];const unsigned n=mode.count;
    p.text(mode.name,0,0,150,kText);
    snprintf(text,sizeof(text),"%u/%u",w.current+1,w.count);p.text(text,150,3,223,kMuted,LV_TEXT_ALIGN_RIGHT,&lv_font_montserrat_14);
    const bool spinning=m.wheelSpinning(),picked=!spinning&&m.wheelPick<n;const unsigned lit=m.wheelLit();
    constexpr int cx=112,cy=116,r=88;
    if(n<2){
        p.rect(cx-r,cy-r,cx+r,cy+r,kPanel,r);
        p.text(n?"只有 1 个选项":"还没有选项",0,cy-30,223,kText,LV_TEXT_ALIGN_CENTER);
        p.text("长按上键说一项，加进来",0,cy-4,223,kMuted,LV_TEXT_ALIGN_CENTER);
    }else{
        // Sector i is centred at the top plus i turns of 360/n degrees, clockwise (LVGL: 0° at 3 o'clock, 90° at 6).
        const float step=360.0f/n;
        for(unsigned i=0;i<n;++i){
            const bool on=(spinning||picked)&&i==lit;
            lv_draw_arc_dsc_t a;lv_draw_arc_dsc_init(&a);a.center={p.area.x1+cx,p.area.y1+cy};
            a.radius=r+(on?5:0);a.width=a.radius;a.start_angle=270+step*i-step/2;a.end_angle=a.start_angle+step;
            a.color=lv_color_hex(spinning||picked?(on?kWheelColors[i%8]:dim(kWheelColors[i%8])):kWheelColors[i%8]);lv_draw_arc(p.layer,&a);
        }
        for(unsigned i=0;i<n;++i){
            const float edge=(270+step*i-step/2)*3.14159265f/180;
            p.line(cx,cy,cx+int(lroundf((r+6)*cosf(edge))),cy+int(lroundf((r+6)*sinf(edge))),kBg,2);
            // The label at the sector's middle, as wide as the sector allows.
            const float mid=(270+step*i)*3.14159265f/180;const int x=cx+int(lroundf(r*0.6f*cosf(mid))),y=cy+int(lroundf(r*0.6f*sinf(mid)));
            const int room=n<=4?80:n<=6?62:46;const char* end=lineEnd(mode.options[i],room);
            fitText(text,sizeof(text),mode.options[i],end-mode.options[i]);
            p.text(text,x-room/2,y-10,x+room/2,(spinning||picked)&&i!=lit?0x5f7f94:kBg,LV_TEXT_ALIGN_CENTER);
        }
        p.rect(cx-22,cy-22,cx+22,cy+22,kBg,22);
        p.text(spinning?"...":"GO",cx-22,cy-9,cx+22,spinning?kMuted:kAccent,LV_TEXT_ALIGN_CENTER,&lv_font_montserrat_14);
    }
    // Under the wheel: a delete to confirm, a notice, the spin, the pick, or what to do.
    if(m.wheelArmed){p.rect(0,214,223,236,kMemoRed,6);snprintf(text,sizeof(text),"再长按下键删除「%s」",mode.name);p.text(text,0,215,223,0xffffff,LV_TEXT_ALIGN_CENTER);}
    else if(*m.lastNotice)paragraph(p,m.lastNotice,214,2,kWarn);
    else if(spinning)p.text("转动中…",0,222,223,kMuted,LV_TEXT_ALIGN_CENTER);
    else if(picked)p.text(mode.options[m.wheelPick],0,210,223,kWheelColors[m.wheelPick%8],LV_TEXT_ALIGN_CENTER,&passport_ui_font_x2);
    else if(n>=2){p.text("按确认转一转",0,212,223,kText,LV_TEXT_ALIGN_CENTER);p.text("长按上键说话添加",0,234,223,0x5f7f94,LV_TEXT_ALIGN_CENTER);}
}
void initWheel(lv_obj_t* screen){
    wheelRoot=lv_obj_create(screen);box(wheelRoot,0,35,240,257,kBg);
    wheelView=lv_obj_create(wheelRoot);box(wheelView,8,2,224,255,kBg);lv_obj_add_event_cb(wheelView,drawWheel,LV_EVENT_DRAW_MAIN,nullptr);
}
void renderWheel(const Model& m){drawModel=&m;lv_obj_invalidate(wheelView);}
// 秒表: the running time in large digits, then the last six laps (total and split), newest first.
void initStopwatch(lv_obj_t* screen){
    swRoot=lv_obj_create(screen);box(swRoot,0,35,240,257,kBg);
    swTime=label(swRoot,0,18,240,&lv_font_montserrat_48,kText);setTextAlign(swTime,LV_TEXT_ALIGN_CENTER,0);
    swState=label(swRoot,0,78,240,LV_FONT_DEFAULT,kMuted);setTextAlign(swState,LV_TEXT_ALIGN_CENTER,0);
    auto* card=lv_obj_create(swRoot);box(card,8,108,224,140,kPanel,12);
    swLaps=label(card,12,8,200,LV_FONT_DEFAULT,kText);lv_label_set_long_mode(swLaps,LV_LABEL_LONG_CLIP);lv_obj_set_height(swLaps,124);
}
void stopwatchText(uint64_t us,char* out,size_t size){
    const uint64_t cs=us/10000;
    if(cs>=360000)snprintf(out,size,"%llu:%02llu:%02llu",cs/360000,cs/6000%60,cs/100%60);else snprintf(out,size,"%02llu:%02llu.%02llu",cs/6000,cs/100%60,cs%100);
}
void renderStopwatch(const Model& m){
    char text[32];const uint64_t us=m.stopwatchUs();stopwatchText(us,text,sizeof(text));setLabel(swTime,text);
    setTextColor(swTime,lv_color_hex(m.swRunning?kAccent:kText),0);
    setLabel(swState,m.swRunning?"计时中":us?"已暂停":"准备就绪");
    static char laps[6*48];laps[0]=0;const unsigned shown=std::min(m.swLapCount,6u);
    for(unsigned i=0;i<shown;++i){
        char total[16],split[16],row[48];stopwatchText(m.swLaps[i],total,sizeof(total));
        // Splits under a minute drop the minutes: +4.10.
        const uint64_t lap=(m.swLaps[i]-(i+1<m.swLapCount?m.swLaps[i+1]:0))/10000;
        if(lap<6000)snprintf(split,sizeof(split),"%llu.%02llu",lap/100,lap%100);else stopwatchText(lap*10000,split,sizeof(split));
        snprintf(row,sizeof(row),"#%u   %s   +%s\n",m.swLapCount-i,total,split);strcat(laps,row);
    }
    setLabel(swLaps,shown?laps:"运行中按「上」计次");
}
// PPT page: a talk timer over the computer link and the last key sent.
void initSlides(lv_obj_t* screen){
    slidesRoot=lv_obj_create(screen);box(slidesRoot,0,35,240,257,kBg);
    auto* chip=lv_obj_create(slidesRoot);box(chip,8,4,84,24,kAccent,12);auto* chipText=label(chip,0,2,84,LV_FONT_DEFAULT,kBg);setTextAlign(chipText,LV_TEXT_ALIGN_CENTER,0);setLabel(chipText,"PPT 翻页");
    slidesLink=label(slidesRoot,100,6,132,LV_FONT_DEFAULT,kMuted);setTextAlign(slidesLink,LV_TEXT_ALIGN_RIGHT,0);
    auto* card=lv_obj_create(slidesRoot);box(card,8,34,224,120,kPanel,12);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(kBorder),0);
    auto* title=label(card,12,8,200,LV_FONT_DEFAULT,kMuted);setLabel(title,"演讲计时");
    slidesTime=label(card,0,32,224,&lv_font_montserrat_48,kText);setTextAlign(slidesTime,LV_TEXT_ALIGN_CENTER,0);
    slidesCaption=label(card,12,92,200,LV_FONT_DEFAULT,kMuted);setTextAlign(slidesCaption,LV_TEXT_ALIGN_CENTER,0);
    auto* lower=lv_obj_create(slidesRoot);box(lower,8,160,224,86,kPanel,12);lv_obj_set_style_border_width(lower,1,0);lv_obj_set_style_border_color(lower,lv_color_hex(kBorder),0);
    slidesStatus=label(lower,12,6,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(slidesStatus,20);
    slidesLast=label(lower,12,32,200,LV_FONT_DEFAULT,kMuted);lv_obj_set_height(slidesLast,20);
    slidesName=label(lower,12,56,200,&lv_font_montserrat_14,kMuted);lv_obj_set_height(slidesName,20);
}
void renderSlides(const Runtime& runtime){
    const auto& m=runtime.model;char text[64];
    const uint64_t elapsed=esp_timer_get_time()/1000000-m.slidesStarted;
    if(elapsed>=3600)snprintf(text,sizeof(text),"%llu:%02llu:%02llu",elapsed/3600,elapsed/60%60,elapsed%60);else snprintf(text,sizeof(text),"%02llu:%02llu",elapsed/60,elapsed%60);
    setLabel(slidesTime,text);setTextColor(slidesTime,lv_color_hex(kAccent),0);
    setLabel(slidesCaption,"退出本页即结束计时");
    const bool off=!runtime.settings().bluetoothEnabled;
    setLabel(slidesLink,off?"蓝牙已关闭":m.slidesReady?"已连接":m.bleEncrypted?"正在连接键盘":m.bleConnected?"正在配对":"未连接");
    setTextColor(slidesLink,lv_color_hex(m.slidesReady?kAccent:kWarn),0);
    setLabel(slidesStatus,off?"● 请在设置中打开蓝牙":m.slidesReady?"● 可以翻页":"● 在电脑蓝牙中添加此设备");
    setTextColor(slidesStatus,lv_color_hex(m.slidesReady?kAccent:kWarn),0);
    const char* names[]={"上一页","下一页","开始放映","黑屏","结束放映"};
    if(!m.slidesPressed)text[0]=0;else snprintf(text,sizeof(text),"%s%s",m.slidesFailed?"未发送：":"已发送：",names[static_cast<unsigned>(m.slidesLast)]);
    setLabel(slidesLast,text);setTextColor(slidesLast,lv_color_hex(m.slidesFailed?kWarn:kText),0);
#ifdef PASSPORT_DEVICE
    setLabel(slidesName,board::bluetoothName());
#else
    setLabel(slidesName,"AI-Passport-C3-Sim");
#endif
}
void renderConnection(const Runtime& runtime){
    const auto& model=runtime.model;const auto app=runtime.active();const bool pairing=*model.pairingCode;
    // Once its list is shown, media reports loading and errors on its own status line.
    const bool connecting=pairing||(model.live&&app!=App::Home&&app!=App::Focus&&app!=App::Settings&&app!=App::Menu&&app!=App::Words&&!(app==App::Radio&&model.mediaCount)&&!keepsContent(app,model)&&model.livePhase!=LivePhase::Ready);
    visible(connectionRoot,connecting);
    if(!connecting)return;
    const bool error=!pairing&&model.livePhase==LivePhase::Error;setLabel(connectionTitle,pairing?"电脑配对":profile(app).title);
    visible(connectionSpinner,!error);visible(connectionError,error);
    lv_arc_set_rotation(connectionSpinner,(esp_timer_get_time()/3000)%360);
    setLabel(connectionMessage,pairing?model.pairingCode:error?model.liveState:model.connectionState);
    // Pages that need the computer explain the first setup; 小智语音 shows how to bind a new board under its code.
    const char* shown=error?model.liveState:model.connectionState;
    const bool setup=needsComputer(app)&&(strstr(shown,"电脑助手")||strstr(shown,"电脑蓝牙"));
    setLabel(connectionHint,pairing?"请核对电脑上的配对码":setup?"首次使用：电脑安装 AI Passport 助手，USB 连接后点「配对设备」"
        :*model.liveLines[0]&&(error||app==App::Radio||app==App::Chat)?model.liveLines[0]:error?"连接失败，请重试":"请稍候");
}
// A window reads "5小时" with "2:10 重置" beside it, or "6天3时重置" when more than a day remains.
void quotaLine(char* name,char* reset,size_t size,const Model::Quota& q){
    if(q.minutes%1440==0)snprintf(name,size,"%lu天",static_cast<unsigned long>(q.minutes/1440));
    else if(q.minutes%60==0)snprintf(name,size,"%lu小时",static_cast<unsigned long>(q.minutes/60));
    else snprintf(name,size,"%lu分钟",static_cast<unsigned long>(q.minutes));
    const auto left=std::max<int64_t>(0,q.resets-time(nullptr));
    if(!q.resets)reset[0]=0;
    else if(left>=86400)snprintf(reset,size,"%lld天%lld时重置",static_cast<long long>(left/86400),static_cast<long long>(left/3600%24));
    else snprintf(reset,size,"%lld:%02lld 重置",static_cast<long long>(left/3600),static_cast<long long>(left/60%60));
}
void renderQuotas(const Model& model){
    char text[64],reset[64];setLabel(dashboardTitle,"AI 额度");
    setLabel(cardTitles[0],"Codex");setLabel(cardTitles[1],"Claude Code");
    for(unsigned n=0;n<4;++n){
        const auto& q=model.quotas[n];const int left=q.known?100-q.used:0;
        if(q.known)quotaLine(text,reset,sizeof(text),q);else{strcpy(text,"暂不可用");reset[0]=0;}
        setLabel(quotaNames[n],text);setLabel(quotaResets[n],reset);
        if(q.known)snprintf(text,sizeof(text),"%d%%",left);else strcpy(text,"--");setLabel(quotaValues[n],text);
        const auto color=!q.known?kMuted:left<=10?kWarn:left<=30?0xffe08a:kAccent;
        setTextColor(quotaValues[n],lv_color_hex(color),0);setBgColor(quotaFills[n],lv_color_hex(color),0);
        setWidth(quotaFills[n],std::max(1,left*192/100));
    }
    setLabel(dashboardFoot,"百分比为剩余额度");
}
void renderDashboard(App app,const Model& model) {
    if(app==App::Codex){renderQuotas(model);return;}
    char text[96];const bool known=!model.live||model.metricsKnown;
    setLabel(dashboardTitle,"电脑状态");setLabel(cardTitles[0],"CPU 占用");
    snprintf(text,sizeof(text),"%u核 负载%.1f",model.cores,double(model.load));setLabel(cardDetails[0],known&&model.cores?text:"");
    visible(cardTrends[0],model.historyCount>1);
    for(unsigned n=0;n<model.historyCount;++n)trendPoints[n]={static_cast<lv_value_precise_t>(n*60/23),static_cast<lv_value_precise_t>(33-model.cpuHistory[n]*33/100)};
    if(model.historyCount>1)lv_line_set_points(cardTrends[0],trendPoints,model.historyCount);
    if(known)snprintf(text,sizeof(text),"%d%%",model.cpu);else strcpy(text,"--");setLabel(cardValues[0],text);
    const auto cpuColor=model.cpu>=90?kWarn:kAccent;
    setTextColor(cardValues[0],lv_color_hex(cpuColor),0);setBgColor(cardBars[0],lv_color_hex(cpuColor),0);setWidth(cardBars[0],std::max(1,model.cpu*192/100));
    // Tiles: memory, disk, battery as a percent with a bar; network as two rates.
    const bool battery=model.battery>=0;
    const int percents[]={model.ram,model.disk,battery?model.battery:100};
    snprintf(text,sizeof(text),"内存 %.0f/%.0fG",double(model.memoryUsed)/(1ULL<<30),double(model.memoryTotal)/(1ULL<<30));setLabel(tileTitles[0],known&&model.memoryTotal?text:"内存");
    snprintf(text,sizeof(text),"磁盘 剩%.0fG",double(model.diskFree)/(1ULL<<30));setLabel(tileTitles[1],known&&model.diskFree?text:"磁盘");
    setLabel(tileTitles[2],!battery?"电源":model.charging?"电池 充电中":"电池");setLabel(tileTitles[3],"网络");
    for(unsigned n=0;n<3;++n){
        const int p=percents[n];const bool low=n==2?battery&&!model.charging&&p<=20:p>=90;
        const auto color=low?kWarn:n==2&&model.charging?0x91c892:n==1?0xc9a7ff:0x91b9ee;
        if(!known)strcpy(text,"--");else if(n==2&&!battery)strcpy(text,"交流电");else snprintf(text,sizeof(text),"%d%%",p);
        if(n==2)setTextFont(tileValues[n],battery||!known?&lv_font_montserrat_24:LV_FONT_DEFAULT,0);
        setLabel(tileValues[n],text);setTextColor(tileValues[n],lv_color_hex(known?color:kMuted),0);
        visible(tileBars[n],known&&(n<2||battery));setBgColor(tileBars[n],lv_color_hex(color),0);setWidth(tileBars[n],std::max(1,p*90/100));
    }
    if(known)snprintf(text,sizeof(text),LV_SYMBOL_UP " %.0f KB/s\n" LV_SYMBOL_DOWN " %.0f KB/s",model.netUp/1024.,model.netDown/1024.);else strcpy(text,"--");
    setLabel(tileValues[3],text);visible(tileBars[3],false);
    const auto days=model.uptime/86400,hours=model.uptime/3600%24;
    if(!known||!model.uptime)text[0]=0;else if(days)snprintf(text,sizeof(text),"已开机 %llu 天 %llu 小时",days,hours);else snprintf(text,sizeof(text),"已开机 %llu 小时 %llu 分",hours,model.uptime/60%60);
    setLabel(dashboardFoot,text);
}
void releasePage(){
    if(display){lv_display_delete(display);display=nullptr;lv_deinit();}
    uiFree=0;
}
void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels) {
    flushPixels+=(area->x2-area->x1+1)*(area->y2-area->y1+1);
    #ifdef PASSPORT_DEVICE
    const auto started=esp_timer_get_time();
    board::draw(area->x1,area->y1,area->x2,area->y2,pixels);
    flushMicros+=esp_timer_get_time()-started;
    #else
    if (capturing) {
        const int width = area->x2 - area->x1 + 1;
        char encoded[648];
        for (int y = area->y1; y <= area->y2; ++y) {
            size_t length = 0;
            mbedtls_base64_encode(reinterpret_cast<uint8_t*>(encoded), sizeof(encoded), &length,
                                  pixels, width * 2);
            encoded[length] = 0;
            printf("ROW %d %d %s\n", static_cast<int>(area->x1), y, encoded);
            pixels += width * 2;
        }
    }
    #endif
    lv_display_flush_ready(disp);
}
}
extern "C" size_t passport_ui_pool_size(){return poolBytes.load();}
// Lends memory to the audio codecs (nullptr takes it back); taking back fails while a codec still uses it.
// The physical build provides it; without one there is nothing to lend to.
extern "C" __attribute__((weak)) bool passport_codec_arena(void*,size_t){return true;}
extern "C" void* passport_ui_pool_alloc(size_t bytes){
#ifdef PASSPORT_LVGL_HEAP
    return uiPool.data(bytes);
#else
    return heap_caps_malloc(bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#endif
}
void uiInit() {
#ifdef PASSPORT_LVGL_HEAP
    // LVGL assumes allocation succeeds; wait outside it when memory is busy.
    if(!uiPool.prepare(poolBytes.load()))return;
#endif
    lv_init();
    lv_tick_set_cb([]()->uint32_t{return static_cast<uint32_t>(esp_timer_get_time()/1000);});
    heading=footer=notice=value=sleepOverlay=topBar=footerRoot=chatRoot=nullptr;footerScrolling=false;
    podRoot=nullptr;
    pomoRoot=dashboard=connectionRoot=mediaRoot=wordsRoot=slidesRoot=alertRoot=statusRoot=nullptr;
    torchRoot=qrRoot=calendarRoot=countRoot=newsRoot=sonarRoot=rateRoot=swRoot=calDetail=histRoot=dailyRoot=nullptr;
    quakeRoot=quizRoot=hnRoot=wifiRoot=soundRoot=nullptr;for(auto& cell:cells)cell=nullptr;
    for(unsigned i=0;i<kRows;++i)rows[i]=rowLabels[i]=rowBadges[i]=rowIcons[i]=rowValues[i]=nullptr;
    for(auto& line:lines)line=nullptr;
    headingMark=hero=heroBadge=heroIcon=heroTrack=heroFill=detail=nullptr;
    const bool compact=guiApp==App::Chat;
    display = lv_display_create(240, 320);
    configASSERT(display);
    // Allocated once at boot and never moved: freeing and re-allocating it per page split the
    // heap and left chat without its 32 KiB stack block. Taller bands mean fewer passes per frame.
    if(!draw)draw = heap_caps_malloc(240 * kDrawRows * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    configASSERT(draw);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw, nullptr, 240 * kDrawRows * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    auto* screen = lv_screen_active();
    box(screen, 0, 0, 240, 320, kBg);
    if(guiApp==App::Home){
        homeUiInit(screen);
        initConnection(screen);initChrome(screen);
        sleepOverlay=lv_obj_create(screen);box(sleepOverlay,0,0,240,320,0);visible(sleepOverlay,false);return;
    }
    headingMark=titleMark(screen,12,45);heading = label(screen, 24, 42, 206, LV_FONT_DEFAULT, kText);
    if(compact){initChat(screen);lines[0]=label(screen, 16, 271, 210, LV_FONT_DEFAULT, kMuted);lv_obj_set_height(lines[0],20);}
    else if(guiApp==App::Radio)initMedia(screen);
    else if(guiApp==App::Podcast)initPodcast(screen);
    else if(guiApp==App::Words)initWords(screen);
    else if(guiApp==App::Slides)initSlides(screen);
    else if(guiApp==App::Status)initStatus(screen);
    else if(guiApp==App::Qr)initQr(screen);
    else if(guiApp==App::Calendar)initCalendar(screen);
    else if(guiApp==App::Countdown)initCountdown(screen);
    else if(guiApp==App::News)initNews(screen);
    else if(guiApp==App::Sonar)initSonar(screen);
    else if(guiApp==App::Rate)initRate(screen);
    else if(guiApp==App::Stopwatch)initStopwatch(screen);
    else if(guiApp==App::History)initHistory(screen);
    else if(guiApp==App::Daily)initDaily(screen);
    else if(guiApp==App::Quake)initQuake(screen);
    else if(guiApp==App::Quiz)initQuiz(screen);
    else if(guiApp==App::Hacker)initHacker(screen);
    else if(guiApp==App::Wifi)initWifi(screen);
    else if(guiApp==App::Sound)initSound(screen);
    else if(guiApp==App::Motion)initMotion(screen);
    else if(guiApp==App::Forecast)initForecast(screen);
    else if(guiApp==App::Noise)initNoise(screen);
    else if(guiApp==App::Alarm)initAlarm(screen);
    else if(guiApp==App::Stock)initStock(screen);
    else if(guiApp==App::Typhoon)initTyphoon(screen);
    else if(guiApp==App::Hot)initHot(screen);
    else if(guiApp==App::Memo)initMemo(screen);
    else if(guiApp==App::Wheel)initWheel(screen);
    else if(guiApp==App::Translate)initTranslate(screen);
    else if(guiApp==App::Guard)initGuard(screen);
    else if(guiApp==App::Todo)initTodo(screen);
    else if(guiApp==App::Ledger)initLedger(screen);
    else if(guiApp==App::Calc)initCalc(screen);
    else if(guiApp==App::Voice)initVoice(screen);
    else if(guiApp==App::Dictation)initDictation(screen);
    else if(guiApp==App::Settings){
        // Status card (icon + main value) above a details card. Only text pages pay for it:
        // the 22 KiB pool must keep room for the pomodoro's rotated layers.
        hero=lv_obj_create(screen);box(hero,8,70,224,96,kPanel,12);lv_obj_set_style_border_width(hero,1,0);lv_obj_set_style_border_color(hero,lv_color_hex(kBorder),0);
        heroBadge=lv_obj_create(hero);box(heroBadge,14,21,52,52,kAccent,26);
        heroIcon=label(heroBadge,0,13,52,&passport_icon_font_24,kBg);setTextAlign(heroIcon,LV_TEXT_ALIGN_CENTER,0);
        // Centred against the icon whether the value is one large line or two text lines.
        value=label(hero,0,0,132,&lv_font_montserrat_24,kAccent);lv_label_set_long_mode(value,LV_LABEL_LONG_WRAP);lv_obj_set_style_max_height(value,56,0);lv_obj_align(value,LV_ALIGN_LEFT_MID,78,-2);
        heroTrack=lv_obj_create(hero);box(heroTrack,78,74,132,6,kBorder,3);heroFill=lv_obj_create(heroTrack);box(heroFill,0,0,1,6,kAccent,3);
        detail=lv_obj_create(screen);box(detail,8,172,224,94,kPanel,12);lv_obj_set_style_border_width(detail,1,0);lv_obj_set_style_border_color(detail,lv_color_hex(kBorder),0);
        for (int i = 0; i < 4; ++i){lines[i] = label(detail, 12, 4 + i * 21, 200, LV_FONT_DEFAULT, i?kMuted:kText);lv_obj_set_height(lines[i],20);}
    }
    if(!compact&&guiApp!=App::Radio&&guiApp!=App::Podcast&&guiApp!=App::Words&&guiApp!=App::Slides&&guiApp!=App::Alert&&guiApp!=App::Status&&guiApp!=App::Torch&&guiApp!=App::Qr&&guiApp!=App::Calendar&&guiApp!=App::Countdown&&guiApp!=App::News&&guiApp!=App::Sonar&&guiApp!=App::Rate&&guiApp!=App::Stopwatch&&guiApp!=App::History&&guiApp!=App::Daily&&guiApp!=App::Menu&&!ownPage(guiApp))notice = label(screen, 12, 270, 218, LV_FONT_DEFAULT, kWarn);
    initConnection(screen);initChrome(screen);
    if(guiApp==App::Alert)initAlert(screen);
    if(guiApp==App::Torch)initTorch(screen);
    if(guiApp==App::Menu)initMenu(screen);
    sleepOverlay=lv_obj_create(screen);box(sleepOverlay,0,0,240,320,0);visible(sleepOverlay,false);
}
void uiResourcesReady(bool ready){resourcesReady=ready;}
bool uiPreparedFor(App app){
    return guiApp==app&&display;
}
bool uiPrepare(App app){
    if(guiApp!=app||!display){
        if(display&&!resourcesReady){setLabel(heading,"正在释放资源");lv_refr_now(display);}
        // UI objects have no network callbacks; release them before waiting for I/O.
        releasePage();guiApp=app;
        if(!resourcesReady){
#ifdef PASSPORT_LVGL_HEAP
            uiPool.reset();
#endif
            return false;
        }
        poolBytes=uiPoolCapacity(app);
#ifdef PASSPORT_LVGL_HEAP
        // LVGL gets the page block back only once the codecs have returned all of it (ui_pool_buffer.hpp).
        if(app!=App::Chat&&!passport_codec_arena(nullptr,0))return false;
#endif
        uiInit();
        if(!display)return false;
#ifdef PASSPORT_LVGL_HEAP
        if(app==App::Chat&&uiPool.data(kPagePool))passport_codec_arena(uiPool.data(kPagePool),kPagePool);
#endif
    }
    bool created=false;
    if(app!=App::Settings&&rows[0])for(unsigned i=0;i<kRows;++i){if(rows[i])lv_obj_delete(rows[i]);rows[i]=rowLabels[i]=rowBadges[i]=rowIcons[i]=rowValues[i]=nullptr;}

    if(pomoRoot&&app!=App::Focus){lv_obj_delete(pomoRoot);pomoRoot=nullptr;}
    const bool dashboardNeeded=app==App::Codex||app==App::Monitor;
    if(dashboard&&(!dashboardNeeded||dashboardApp!=app)){lv_obj_delete(dashboard);dashboard=nullptr;}
    if(app==App::Focus&&!pomoRoot){initPomodoro(lv_screen_active());created=true;}
    if(dashboardNeeded&&!dashboard){initDashboard(lv_screen_active(),app);created=true;}
    if(app==App::Settings&&!rows[0]){
        auto* screen=lv_screen_active();
        const bool menu=false;const int count=kRows,top=68,step=29,height=27,badge=23;  // above the notice at y=270
        for (int i = 0; i < count; ++i) {
            rows[i] = lv_obj_create(screen);
            box(rows[i], 8, top + i * step, 224, height, kPanel, 8);
            rowBadges[i]=lv_obj_create(rows[i]);box(rowBadges[i],5,(height-badge)/2,badge,badge,kAccent,6);
            rowIcons[i]=label(rowBadges[i],0,(badge-15)/2,badge,&lv_font_montserrat_14,kBg);setTextAlign(rowIcons[i],LV_TEXT_ALIGN_CENTER,0);
            // A long WiFi name ends in dots instead of wrapping out of its row.
            rowLabels[i] = label(rows[i], 36, (height-20)/2, menu?180:128, LV_FONT_DEFAULT, kText);
            lv_label_set_long_mode(rowLabels[i],LV_LABEL_LONG_MODE_DOTS);lv_obj_set_height(rowLabels[i],20);
            if(!menu){rowValues[i]=label(rows[i],126,(height-20)/2,88,LV_FONT_DEFAULT,kMuted);setTextAlign(rowValues[i],LV_TEXT_ALIGN_RIGHT,0);}
        }
        created=true;
    }
    if(created){lv_obj_move_foreground(connectionRoot);lv_obj_move_foreground(topBar);lv_obj_move_foreground(footerRoot);lv_obj_move_foreground(sleepOverlay);trackUiMemory();}
    return true;
}
void uiRender(const Runtime& runtime) {
    if(!uiPrepare(runtime.active()))return;
    const auto app = runtime.active();
    const auto& model = runtime.model;
    renderChrome(runtime);
    if(app==App::Home){
        // The photo screen is full-bleed; status and key bars return on the other screens.
        const bool photo=model.homeScreen==HomeScreen::Photo;visible(topBar,!photo);visible(footerRoot,!photo);
        homeUiRender(runtime);renderConnection(runtime);visible(sleepOverlay,runtime.power(esp_timer_get_time()/1000000)!=Power::Awake);lv_timer_handler();lv_refr_now(display);trackUiMemory();return;}
    if(app==App::Alert){if(renderAlert(model))return;lv_timer_handler();lv_refr_now(display);trackUiMemory();return;}
    if(app==App::Torch){renderTorch(model);lv_timer_handler();lv_refr_now(display);trackUiMemory();return;}
    setLabel(heading, profile(app).title);
    if(app==App::Chat&&model.live){char title[120];snprintf(title,sizeof(title),"%s · %s","小智语音",model.livePhase==LivePhase::Loading?model.connectionState:model.liveState);setLabel(heading,title);}
    visible(sleepOverlay,runtime.power(esp_timer_get_time()/1000000)!=Power::Awake);
    if(app==App::Chat)renderChat(model);
    const bool settingsList=app==App::Settings&&model.settingsPage==SettingsPage::List;
    // Settings > WiFi without the setup hotspot: 添加 WiFi, then the saved networks in the same rows.
    const bool wifiList=app==App::Settings&&model.settingsPage==SettingsPage::Wifi&&!model.wifiSetup;
    const bool showDashboard=app==App::Codex||app==App::Monitor;
    const bool showHero=app!=App::Menu&&app!=App::Chat&&app!=App::Focus&&app!=App::Radio&&app!=App::Podcast&&app!=App::Words&&app!=App::Slides&&app!=App::Status&&app!=App::Qr&&app!=App::Calendar&&app!=App::Countdown&&app!=App::News&&app!=App::Sonar&&app!=App::Rate&&app!=App::Stopwatch&&app!=App::History&&app!=App::Daily&&!ownPage(app)&&!settingsList&&!wifiList&&!showDashboard;
    visible(pomoRoot,app==App::Focus);visible(dashboard,showDashboard);
    const bool ownTitle=app==App::Focus||app==App::Menu||app==App::Radio||app==App::Podcast||app==App::Words||app==App::Slides||app==App::Status||app==App::Qr||app==App::Calendar||app==App::Countdown||app==App::News||app==App::Sonar||app==App::Rate||app==App::Stopwatch||app==App::History||app==App::Daily||ownPage(app)||showDashboard;
    visible(heading,!ownTitle);visible(headingMark,!ownTitle);
    visible(notice,app!=App::Focus&&app!=App::Chat&&!showDashboard);
    if(showDashboard)renderDashboard(app,model);
    visible(hero,showHero);visible(detail,showHero);visible(heroTrack,false);
    if(showHero&&hero){
        const auto badge=app==App::Settings?settingsBadge(model.settingsPage):appBadge(app);
        setBgColor(heroBadge,lv_color_hex(badge.color),0);setLabel(heroIcon,badge.symbol);
    }
    setLabel(value, "");
    for (int i = 0; i < kRows; ++i) visible(rows[i], settingsList||(wifiList&&i<=model.wifiSavedCount));
    for(unsigned i=0;i<4;++i)setLabel(lines[i],"");
    if(settingsList){
        char values[kRows][48];
        strcpy(values[0],model.wifiConnected?"已连接":"未连接");
        strcpy(values[1],!runtime.settings().bluetoothEnabled?"已关闭":model.bleConnected?"已连接":"未连接");
        snprintf(values[2],sizeof(values[2]),"%u%%",runtime.settings().brightness);
        snprintf(values[3],sizeof(values[3]),"%u%%",model.volume);
        if(runtime.settings().idleSeconds)snprintf(values[4],sizeof(values[4]),"%lu 分钟",static_cast<unsigned long>(runtime.settings().idleSeconds/60));else strcpy(values[4],"永不");
        strcpy(values[5],model.wakeWord?"已开启":"已关闭");
        // The city without its province ("杭州 · 浙江").
        snprintf(values[6],sizeof(values[6]),"%s",*model.cityName?model.cityName:"未设置");if(char* dot=strstr(values[6]," · "))*dot=0;
        const char* names[kRows]={"WiFi 配置","蓝牙连接","屏幕亮度","默认音量","自动息屏","语音唤醒","所在城市"};
        for(unsigned i=0;i<kRows;++i){
            const bool selected=i==model.settingsSelection;
            setLabel(rowLabels[i],names[i]);setLabel(rowValues[i],values[i]);setBadge(rowBadges[i],rowIcons[i],settingsBadge(static_cast<SettingsPage>(i+1)),selected);
            setBgColor(rows[i],lv_color_hex(selected?kAccent:kPanel),0);
            setTextColor(rowLabels[i],lv_color_hex(selected?kBg:kText),0);setTextColor(rowValues[i],lv_color_hex(selected?kBg:kMuted),0);
        }
    }else if(wifiList){
        setLabel(heading,"WiFi 配置");
        for(unsigned i=0;i<=model.wifiSavedCount&&i<unsigned(kRows);++i){
            const bool selected=i==model.wifiRow,current=i&&model.wifiConnected&&!strcmp(model.wifiSaved[i-1],runtime.settings().wifiSsid);
            setLabel(rowLabels[i],i?model.wifiSaved[i-1]:"添加 WiFi");setLabel(rowValues[i],i?(current?"已连接":""):"手机配网");
            setBadge(rowBadges[i],rowIcons[i],i?Badge{LV_SYMBOL_WIFI,current?0x4bffc4u:0x5fd3ffu}:Badge{LV_SYMBOL_PLUS,0xffc861},selected);
            setBgColor(rows[i],lv_color_hex(selected?(model.wifiArmed?kWarn:kAccent):kPanel),0);
            setTextColor(rowLabels[i],lv_color_hex(selected?kBg:kText),0);setTextColor(rowValues[i],lv_color_hex(selected?kBg:kMuted),0);
        }
    }else if (app == App::Menu) {renderMenu(runtime);
    } else if(app==App::Focus){renderPomodoro(model);
    } else if(app==App::Radio){renderMedia(model);
    } else if(app==App::Podcast){renderPodcast(model);
    } else if(app==App::Words){renderWords(model);
    } else if(app==App::Slides){renderSlides(runtime);
    } else if(app==App::Status){renderStatus(model);
    } else if(app==App::Qr){renderQr(runtime);
    } else if(app==App::Calendar){renderCalendar(model);
    } else if(app==App::Countdown){renderCountdown(model);
    } else if(app==App::News){renderNews(model);
    } else if(app==App::Sonar){renderSonar(runtime);
    } else if(app==App::Rate){renderRate(model);
    } else if(app==App::Stopwatch){renderStopwatch(model);
    } else if(app==App::History){renderHistory(model);
    } else if(app==App::Daily){renderDaily(model);
    } else if(app==App::Quake){renderQuake(model);
    } else if(app==App::Quiz){renderQuiz(model);
    } else if(app==App::Hacker){renderHacker(model);
    } else if(app==App::Wifi){renderWifi(model);
    } else if(app==App::Motion){renderMotion(model);
    } else if(app==App::Forecast){renderForecast(model);
    } else if(app==App::Noise){renderNoise(model);
    } else if(app==App::Alarm){renderAlarm(model);
    } else if(app==App::Stock){renderStock(model);
    } else if(app==App::Typhoon){renderTyphoon(model);
    } else if(app==App::Hot){renderHot(model);
    } else if(app==App::Memo){renderMemo(model);
    } else if(app==App::Dictation){renderDictation(model);
    } else if(app==App::Wheel){renderWheel(model);
    } else if(app==App::Translate){renderTranslate(model);
    } else if(app==App::Guard){renderGuard(model);
    } else if(app==App::Todo){renderTodo(model);
    } else if(app==App::Ledger){renderLedger(model);
    } else if(app==App::Calc){renderCalc(model);
    } else if(app==App::Voice){renderVoice(model);
    } else if(app==App::Sound){renderSound(model);
    } else if(model.live && app!=App::Settings) {
        if(value)setTextFont(value,(app==App::Home||app==App::Focus||app==App::Codex) ? &lv_font_montserrat_24 : LV_FONT_DEFAULT,0);
        setLabel(value,model.liveValue);
        for(unsigned i=0;i<4;++i) setLabel(lines[i],model.liveLines[i]);
    } else {
        switch (app) {
        case App::Codex:
            setLabel(value, model.quotaKnown ? "62%" : "--");
            setLabel(lines[0], model.quotaKnown ? "当前用量：62%" : "当前用量暂不可用");
            setLabel(lines[1], "2 个任务正在运行");
            setLabel(lines[2], "1 个任务等待确认");
            break;
        case App::Monitor:
            setLabel(value, "CPU 37%");
            setLabel(lines[0], "内存 62%   网络 2.4MB/s");
            setLabel(lines[1], "进程 1：Codex");
            setLabel(lines[2], "进程 2：Chrome");
            break;
        case App::Home:
            setLabel(value, "10:20");
            setLabel(lines[0], "星期二  2026-10-20");
            setLabel(lines[1], model.weather);
            setLabel(lines[2], "空气良 · 10 分钟前更新");
            break;
        case App::Settings: {
            const auto page=model.settingsPage;
            const bool number=page==SettingsPage::Brightness||page==SettingsPage::Volume;
            if(value)setTextFont(value,number?&lv_font_montserrat_24:LV_FONT_DEFAULT,0);
            for(unsigned i=0;i<4;++i)setTextColor(lines[i],lv_color_hex(i?kMuted:kText),0);
            if(page==SettingsPage::Wifi&&!wifiList){
                setLabel(heading,"WiFi 配置");
#ifdef PASSPORT_DEVICE
                if(board::provisioning()){setLabel(value,"手机连接免密码热点");setLabel(lines[0],board::apName());setLabel(lines[1],"http://192.168.8.1");setLabel(lines[2],model.wifiSetupStatus);}else
#endif
                {setLabel(value,model.wifiConnected?"WiFi 已连接":"WiFi 未连接");setLabel(lines[0],runtime.settings().wifiSsid);setLabel(lines[1],"确认开启手机配网");}
                setLabel(lines[3],"网页：WiFi·身份卡·蓝牙·收听");
            }else if(page==SettingsPage::Bluetooth){
                setLabel(heading,"蓝牙连接");
                setLabel(value,runtime.connected()?"USB 已连接":!runtime.settings().bluetoothEnabled?"蓝牙已关闭":model.bleEncrypted?"蓝牙已连接":model.bleConnected?"正在加密配对":"等待电脑助手连接");
#ifdef PASSPORT_DEVICE
                setLabel(lines[0],board::bluetoothName());
#else
                setLabel(lines[0],"AI-Passport-C3-Sim");
#endif
                setLabel(lines[1],runtime.connected()?"拔线后自动使用蓝牙":"电脑开启蓝牙及电脑助手");setLabel(lines[2],model.bleConnected&&!model.bleEncrypted?"一直不通：电脑移除后重连":"首次连接需同意电脑配对");setLabel(lines[3],"进入电脑功能后自动连接");
            }else if(page==SettingsPage::City){
                // Two ways to set it: say the city, or look it up by the internet connection's IP.
                setLabel(heading,"所在城市");
                setLabel(value,model.listening()?"正在听…":model.voiceBusy()?"正在识别…":*model.cityName?model.cityName:"未设置");
                setLabel(lines[0],model.cityRow?"○  语音输入城市":"●  语音输入城市");setLabel(lines[1],model.cityRow?"●  按 IP 定位":"○  按 IP 定位");
                setTextColor(lines[0],lv_color_hex(model.cityRow?kMuted:kAccent),0);setTextColor(lines[1],lv_color_hex(model.cityRow?kAccent:kMuted),0);
                setLabel(lines[2],model.listening()?"说城市名，说完按确认":model.cityRow?"开了代理或 VPN 会不准":"说城市名，如「苏州」");setLabel(lines[3],"天气和收音机按这个城市");
            }else if(page==SettingsPage::Wake){
                // Three switches, the one picked in the accent colour: the wake word (it keeps the microphone, codec and
                // CPU running and rules out deep sleep on the home page and the menu), 说功能名跳转 and 打开 Mac 程序.
                const unsigned at=model.wakeRow;const bool on[3]{model.wakeWord,model.voiceJump,model.macOpen};
                const char* names[3]{"你好小智唤醒","说功能名跳转","打开 Mac 程序"};
                setLabel(heading,"语音唤醒");setLabel(value,on[at]?"已开启":"已关闭");
                for(unsigned i=0;i<3;++i){
                    char row[64];snprintf(row,sizeof(row),"%s  %s：%s",i==at?"●":"○",names[i],on[i]?"开":"关");
                    setLabel(lines[i],row);setTextColor(lines[i],lv_color_hex(i==at?kAccent:kMuted),0);
                }
                setLabel(lines[3],at==2?"说「打开 Mac 的微信」":at?"说「打开汇率」就进汇率":"麦克风常开，待机明显变短");
            }else if(page==SettingsPage::Idle){
                // Two rows like 语音唤醒; the time is changed in place after Confirm.
                const bool usb=model.idleRow==1,editing=model.idleRow==2;
                char minutes[24],row[2][64];
                if(model.settingsValue)snprintf(minutes,sizeof(minutes),"%lu 分钟",static_cast<unsigned long>(model.settingsValue));else strcpy(minutes,"永不");
                setLabel(heading,"自动息屏");setLabel(value,usb?(model.usbAwake?"已开启":"已关闭"):minutes);
                snprintf(row[0],sizeof(row[0]),"%s  息屏时间：%s",usb?"○":"●",minutes);
                snprintf(row[1],sizeof(row[1]),"%s  连电脑时常亮：%s",usb?"●":"○",model.usbAwake?"开":"关");
                setLabel(lines[0],row[0]);setLabel(lines[1],row[1]);
                setTextColor(lines[0],lv_color_hex(usb?kMuted:kAccent),0);setTextColor(lines[1],lv_color_hex(usb?kAccent:kMuted),0);
                setLabel(lines[2],editing?"上下调整时间，0 表示永不":usb?(model.usbAwake?"连着电脑一直亮屏":"连着电脑也按时间熄屏"):"确认后上下调整");
                setLabel(lines[3],usb?"连电脑时只熄屏，不会睡眠":editing?"确认保存 · 长按确认取消":"充电头供电照常熄屏");
            }else if(number){
                setLabel(heading,page==SettingsPage::Brightness?"屏幕亮度":"默认音量");
                char text[16];snprintf(text,sizeof(text),"%lu%%",static_cast<unsigned long>(model.settingsValue));setLabel(value,text);
                visible(heroTrack,true);setWidth(heroFill,std::max<int>(1,model.settingsValue*132/100));
                setLabel(lines[0],"上下调整 · 确认保存");setLabel(lines[1],"长按确认取消并返回");
            }
            break;
        }
        default: break;
        }
    }
    setLabel(notice, runtime.lastResult != Result::Ok ? "切换未完成，请重试"
        : model.lastNotice);
    if(app==App::Chat){char status[192];snprintf(status,sizeof(status),"%s音量 %u%%",model.live&&*model.liveLines[2]?model.liveLines[2]:"",model.volume);setLabel(lines[0],status);}
    if(showDashboard){visible(value,false);for(auto* line:lines)visible(line,false);}
    renderConnection(runtime);
    lv_timer_handler();lv_refr_now(display);trackUiMemory();
}
unsigned uiTextLines(const char* s,int width){unsigned n=0;while(*s){s=lineEnd(s,width);++n;}return n;}
unsigned uiWrappedLines(const char* s){unsigned n=0;for(;;){while(*s==' ')++s;if(!*s)return n;s=wrapEnd(s,224);++n;}}
unsigned uiMemoryFree(){return uiFree.load();}
unsigned uiFlushPixels(){return flushPixels.load();}
unsigned uiFlushMicros(){return flushMicros.load();}
unsigned uiMemoryPeak(){return uiPeak.load();}
bool uiAnimating(const Runtime& runtime) {return runtime.power(esp_timer_get_time()/1000000)==Power::Awake&&(runtime.active()==App::Alert||(runtime.active()==App::Torch&&esp_timer_get_time()/1000000-runtime.model.torchChanged<3)||(runtime.active()==App::Stopwatch&&runtime.model.swRunning)||(runtime.active()==App::News&&*runtime.model.liveLines[0])||runtime.active()==App::Sound||(runtime.active()==App::Memo&&(runtime.model.listening()||(runtime.model.memoSavedAt&&esp_timer_get_time()/1000000-runtime.model.memoSavedAt<5)))||((runtime.active()==App::Status||runtime.active()==App::Translate)&&runtime.model.listening())||(runtime.active()==App::Todo&&(runtime.model.listening()||runtime.model.todoRinging))||((runtime.active()==App::Ledger||runtime.active()==App::Calc||runtime.active()==App::Voice||runtime.active()==App::Alarm||runtime.active()==App::Countdown||speaksToo(runtime.active()))&&runtime.model.listening())||(runtime.active()==App::Wheel&&(runtime.model.listening()||(runtime.model.wheelSpunAt&&uint64_t(esp_timer_get_time())-runtime.model.wheelSpunAt<Model::kWheelSpin+500000)))||(runtime.active()==App::Dictation&&(runtime.model.listening()||(runtime.model.dictationAt&&esp_timer_get_time()/1000000-runtime.model.dictationAt<5)))||footerScrolling||*runtime.model.pairingCode||(runtime.model.live&&runtime.model.livePhase==LivePhase::Loading&&runtime.active()!=App::Home&&runtime.active()!=App::Focus&&runtime.active()!=App::Menu&&runtime.active()!=App::Settings&&runtime.active()!=App::Words&&!keepsContent(runtime.active(),runtime.model)));}
void uiCapture(const char* name) {
#ifdef PASSPORT_DEVICE
    (void)name;return;
#endif
    printf("FRAME_BEGIN %s 240 320\n", name);
    capturing = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    capturing = false;
    printf("FRAME_END %s\n", name);
}
void uiCheck() {
    lv_mem_monitor_t memory{};
    lv_mem_monitor(&memory);
    configASSERT(lv_mem_test() == LV_RESULT_OK);
    configASSERT(passport_ui_font_covers_all());
    puts("UI_FONT all_configured_glyphs_present=1");
    printf("UI_HEAP total=%u free=%u largest=%u\n", static_cast<unsigned>(memory.total_size),
           static_cast<unsigned>(memory.free_size), static_cast<unsigned>(memory.free_biggest_size));
}
void uiShutdown() {
    releasePage();
    heap_caps_free(draw); draw = nullptr;
}
} // namespace passport
