#pragma once
#include "audio_pipe.hpp"
#include "esp_http_client.h"
#include "esp_transport.h"
#include "media_catalog.hpp"
#include "podcast_catalog.hpp"
#include "stock_catalog.hpp"
#include "hot_board.hpp"
#include "train.hpp"
#include <atomic>

struct cJSON;
namespace passport {
// 中英翻译: what was heard and its translation (toEnglish: Chinese was heard), by engine 1 腾讯翻译君 / 2 MyMemory, 0 none.
struct TranslationText{char source[600],result[900];bool toEnglish;uint8_t engine;};
// 小智 MCP: open the page named, then (text not empty) say text to it as if heard there.
struct McpJob{char page[32],text[128];};
class NativeServices {
    struct Job{App app{};uint32_t generation{},request{};char operation[32]{};};  // 32: 高铁查询's "t:<train>:<day>:<from>:<to>"
    using Reply=bool(*)(void*,const char*);
    AudioPipe& audio_;Reply reply_;void* context_;
    QueueHandle_t actions_{};TaskHandle_t task_{};
    Job pending_{},current_{};bool pendingReady_{},network_{},memoryWaiting_{};int64_t pendingSince_{};
    std::atomic<uint32_t> desired_{},owner_{},macroRequest_{};
    std::atomic<bool> finished_{},allowed_{},ready_{},busy_{},online_{};
    std::atomic<unsigned> httpCount_{},httpStatus_{},weatherUpdates_{},chatTx_{},chatRx_{},mediaBytes_{},stackFree_{};
    std::atomic<unsigned> ttsStarts_{},ttsStops_{},listenStarts_{};
    std::atomic<unsigned> httpRetries_{},cloudChecks_{},connectAttempts_{},readFailHeap_{};
    std::atomic<bool> cloudCheckOk_{};
    std::atomic<unsigned> pumpUs_{},decodeUs_{},readUs_{},rxGapUs_{};
    std::atomic<int> error_{},tlsError_{},tlsFlags_{},socketError_{},tlsResult_{};
    struct WarnSeen{char id[32];bool covers;} warnSeen_[6]{};unsigned warnSeenNext_{};  // provincial warnings already read
    char weather_[4][160]{};WeatherReading weatherReading_;std::atomic<uint64_t> weatherAt_{};uint32_t weatherPrefs_{};
    uint8_t mediaIndex_{},mediaCount_{};
    esp_transport_handle_t ws_{},ssl_{};
    char session_[96]{},stt_[160]{},answer_[480]{};
    unsigned speechTurn_{};
    std::atomic<bool> listening_{},speaking_{};
    uint32_t rate_{24000};bool autoListening_{},dropTts_{},resumeListening_{};
    // 语音笔记: Confirm came before the connection was ready; the cloud is turning speech into text (asr_); the cloud
    // closed an idle connection; the last status was an error. memoHeard_: what it heard, held (the task waits)
    // until LiveServices has saved it, since a status update may overwrite the one before it in the queue.
    bool wantListen_{},asr_{},idleDrop_{},failed_{};int64_t listenAt_{},askedAt_{};std::atomic<const char*> memoHeard_{};
    // 中英翻译: a translation waits in page_.translation until LiveServices has taken it; a long Up asked to read it aloud.
    std::atomic<bool> translated_{},sayWanted_{};
    // 小智 MCP: the volume and the brightness the chat's model asked for, until LiveServices (main task) applies them; -1 none.
    std::atomic<int> mcpVolume_{-1},mcpBrightness_{-1};
    // 小智 MCP: a page to open, or (text) one to say something to, done by LiveServices; the task waits for its answer.
    McpJob mcpJob_{};std::atomic<bool> mcpWaiting_{};bool mcpOk_{};char mcpResult_[96]{};
    void mcp(const cJSON* payload);
    // 语音指令: LiveServices did not know the phrase (askModel_), so the connection stays open for 小智's model from
    // modelAt_ (0 = not waiting); toolNote_ is what its tools did.
    std::atomic<bool> askModel_{};int64_t modelAt_{};char toolNote_[96]{};
    void note(const char*);void modelDone();
    static void run(void*);
    static void audioOutput(void*,uint8_t,const uint8_t*,size_t);
    bool cancelled()const{return desired_.load()!=current_.generation||(current_.app==App::Macro&&macroRequest_.load()!=current_.request);}
    // 语音笔记 and 状态牌: speech to text only, as XiaoZhi's second device (the chat keeps its own history); also one
    // phrase spoken on 汇率, 自选股 or 所在城市 ("speak").
    bool transcribing()const{return keepsListening(current_.app)||speaksOnce(current_.app)||current_.app==App::Voice||!strcmp(current_.operation,"speak");}
    // 自选股 and 所在城市: the phrase heard, which this task looks up itself; what came of it goes out with the next
    // update as "notice" (and "select": the security added, "city": the city set).
    char spoken_[160]{},notice_[160]{},city_[80]{};int select_{-1};
    char sayText_[256]{};  // 飞书提醒: the message to read aloud, set before its "say" request
    void sayText();
    void addStock(const char*);void macro();void setCity(const char*);void locate();
    void work();void home();void chat();void media();void podcast();void words();void news();void rates();void history();void daily();void quake();void hacker();void quiz();void forecast();void stock();void typhoon();void train();bool trainFetch();void homeTrain();int16_t trainLate(const char* station,uint32_t date,unsigned minutes,bool leaving);void hot();bool newsList();void memo();void translate();void sayTranslation();bool hotBoard();bool air(double,double,WeatherForecast&);bool warnings(const MediaPrefs&,WeatherWarning*,uint8_t&,unsigned);bool warningText(const char*,char*,size_t);
    bool playMp3(esp_http_client_handle_t client,uint8_t* bytes,size_t size,int count);
    // 财经新闻 while its page is open (the task's own block): publish() sends the six titles and item newsIndex_
    // in full, which stays under the 2 KiB message (a longer one is dropped, leaving the page loading).
    struct NewsItem{char time[6];char title[88];char digest[720];};NewsItem* news_{};unsigned newsCount_{},newsIndex_{},newsPage_{1};
    // 历史今日 and 每日一句 as last fetched; titles are capped so 16 events stay under the 2 KiB update.
    struct EventItem{int16_t year;uint8_t type;char title[64];};EventItem events_[16]{};unsigned eventCount_{};
    struct Daily{char en[224];char cn[192];char date[12];unsigned back;};Daily daily_{};
    // 地震速报, Hacker News, 知识问答, 播客 and 天气预报 share one block: only the open page's task fills it.
    struct QuakeItem{uint32_t at;char place[48];uint8_t mag;uint16_t depth;};
    struct StoryItem{char title[90];char site[26];uint16_t points,comments;uint32_t at;};
    struct Quiz{uint32_t serial;char question[256];char answers[4][72];char category[40];char level[8];uint8_t correct;};
    struct PodcastRow{char name[40];char note[48];};
    // 高铁查询 while its page is open (the task's own block): every stop's 检票口, 候车室 and 出站口; trainStops_: the
    // next update sends the stops too (only after a fetch: a picked stop sends just its own).
    struct TrainExtra{char gate[64],wait[32],exit[48];};TrainExtra* trainExtra_{};bool trainStops_{};
    // The home page: today's trip's 检票口 as last read (homeTrip_.code empty: none), sent with every home update.
    struct HomeTrip{char code[8];uint32_t date;uint8_t fromAt,toAt;} homeTrip_{};char homeGate_[48]{};
    // 百度热搜 while its page is open (the task's own block): full titles and summaries, item hotIndex_ on show.
    HotItem* hot_{};unsigned hotCount_{},hotIndex_{};uint32_t hotUpdated_{};
    union Page{TranslationText translation;QuakeItem quakes[16];StoryItem stories[10];Quiz quiz;PodcastRow podcasts[PodcastList::kMax];WeatherForecast forecast;TyphoonView typhoon;MacroView macro;
        // 高铁查询: the train fetched (stops as TrainView's), the stop picked, the trip's stops and how late it is there.
        struct Train{char code[8];uint32_t date;char style[16];uint8_t count,total,pick,from,to;int16_t late[2];TrainStop stops[kTrainStops];} train;
        // 自选股: the quotes, and for row chartFor (-1 = none) its chart scaled 0-999 between low and high.
        struct Stocks{struct Row{char name[24];char code[10];char tag[4];uint8_t decimals;float v[8];} rows[StockList::kMax];unsigned count;uint32_t updated;
            int chartFor;bool days;unsigned chartCount,chartTotal;int16_t chart[ChartPoints::kMax];float low,high;char from[8],mid[16],to[12];} stocks;} page_{};unsigned quakeCount_{},storyCount_{},podcastCount_{};
    // The radio or a podcast is playing; a podcast's player is open, at podcastPos_ of podcastDur_ seconds.
    bool playing_{},podcastPlayer_{};uint32_t podcastPos_{},podcastDur_{};
    // 选期: episodesCount_ episodes (newest first) in page_.podcasts, from the episodesFirst_-th newest of episodesTotal_;
    // episodesHeard_ is the row being heard, -1 = not on this page.
    unsigned episodesCount_{},episodesFirst_{},episodesTotal_{};int episodesHeard_{-1};
    RateBoard rateBoard_{};double rates_[RateBoard::kTargets]{};uint32_t rateUpdated_{};
    // A sink takes the body as it arrives instead of a buffer (nullptr data first: start over); false ends the read.
    using Sink=bool(*)(void*,const char*,size_t);
    // post: a JSON body to POST instead of a GET. userAgent is an endpoint-specific compatibility override.
    bool syncClock();bool fetch(const char*,char*,size_t,Sink sink=nullptr,void* context=nullptr,const char* post=nullptr,const char* userAgent=nullptr,const char* const* headers=nullptr);bool retryPause(unsigned);
    // XiaoZhi's settings check (OTA) for a device without the old firmware's identity: the server address and token,
    // after showing an activation code and waiting until the user binds it. xiaozhiPost returns the HTTP status or -1.
    int xiaozhiPost(const char* url,const uint8_t* mac,const char* client,const char* body,char* output,size_t capacity);
    bool cloudIdentity(const uint8_t* mac,const char* client,char* url,size_t urlSize,char* token,size_t tokenSize,bool probe);
    // offset > 0 asks for the rest of the file from that byte (HTTP Range); a server may still send all of it (200).
    // secure allows HTTPS, for a task with the stack for a TLS handshake (中英翻译's 32 KiB).
    esp_http_client_handle_t openStream(const char* url,int timeoutMs=3000,uint32_t offset=0,bool secure=false);
    void publish(const Job&,const char*,const char* phase="ready",const char* value="",const char* const* lines=nullptr);
    void status(const char* state,const char* phase="ready");
    bool text(const char* type,const char* state=nullptr,const char* mode=nullptr);
    bool controlAudio(uint8_t);void beginListen(bool interrupted=false);void toggleChat();void endListen();void closeSocket();
public:
    NativeServices(AudioPipe& audio,Reply reply,void* context);
    void network(bool ready){network_=ready;}
    bool request(App,uint32_t,uint32_t,const char*);
    void cancel(uint32_t generation){desired_=generation;pendingReady_=false;}
    void poll();
    bool idle()const{return task_==nullptr;}
    const char* memoHeard()const{return memoHeard_.load();}  // "" when nothing was heard
    void memoTaken(){memoHeard_=nullptr;}
    void askModel(){askModel_=true;}  // before memoTaken()
    int takeVolume(){return mcpVolume_.exchange(-1);}
    int takeBrightness(){return mcpBrightness_.exchange(-1);}
    const McpJob* mcpJob()const{return mcpWaiting_.load()?&mcpJob_:nullptr;}
    void mcpDone(bool ok,const char* result){mcpOk_=ok;snprintf(mcpResult_,sizeof(mcpResult_),"%s",result);mcpWaiting_=false;}
    void speakText(const char* text);  // before LiveServices asks 飞书提醒's "say"
    const TranslationText* translated()const{return translated_.load()?&page_.translation:nullptr;}
    void translationTaken(){translated_=false;}
    bool busy()const{return task_||pendingReady_;}
    // The home page keeps Bluetooth up; its job asks for the memory only when it really goes online (HTTPS).
    bool online()const{return online_;}
    bool allowLocation(bool);
    void cityName(char*,size_t)const;  // the city set, as Settings shows it ("" when none)
    void snapshot(char*,size_t)const;
};
}
