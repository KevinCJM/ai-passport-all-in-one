#include "tool_tasks.hpp"
#include "board.hpp"
#include "pitch.hpp"
#include "noise_gen.hpp"
#include "identity.hpp"
#include "dsps_fft2r.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <new>

namespace passport::tools {
namespace {
// Task entry: the body returns first, so its locals (unique_ptr buffers) are destroyed, and only then the task
// deletes itself. vTaskDelete(nullptr) never returns: a body ending in it leaked its buffers (12 KB per 声音 visit).
template<void (*Body)(void*)> void taskMain(void* argument) { Body(argument); vTaskDelete(nullptr); }
// ---- WiFi 工具箱 ----
// The page's choices go in, the results come out; each change of results bumps version.
struct WifiState {
    uint8_t tab{}, action{}; bool connected{}; uint8_t wakeMac[6]{};
    Model::WifiAp aps[16]{}; uint8_t apCount{};
    Model::PingRow pings[3]{}; uint8_t pingCount{}; uint16_t dnsMs{};
    Model::LanDevice lan[32]{}; uint8_t lanCount{}, lanSelf{255}, lanGateway{255};
    char note[64]{}; bool busy{}; uint32_t wakeSentAt{}, version{};
};
// Allocated while the page is open; freed once its task has ended.
WifiState* wifiShared; StaticSemaphore_t wifiLockStorage; SemaphoreHandle_t wifiLock;
std::atomic<bool> wifiStop{}, wifiRunning{}; esp_netif_t* wifiNetif;
template<class F> void withWifi(F f) { xSemaphoreTake(wifiLock, portMAX_DELAY); f(*wifiShared); xSemaphoreGive(wifiLock); }
void wifiNote(const char* text, bool busy) { withWifi([&](WifiState& s) { snprintf(s.note, sizeof(s.note), "%s", text); s.busy = busy; ++s.version; }); }
// A long job gives up when the page leaves its tab (it runs again on return).
bool leftTab(uint8_t tab) { uint8_t now = tab; withWifi([&](WifiState& s) { now = s.tab; }); return wifiStop || now != tab; }

void scanNetworks() {
    wifiNote("正在扫描周围的 WiFi…", true);
    auto records = std::unique_ptr<wifi_ap_record_t[]>(new (std::nothrow) wifi_ap_record_t[16]);
    wifi_scan_config_t config{}; config.show_hidden = true; uint16_t count = 16;
    if (!records || esp_wifi_scan_start(&config, true) != ESP_OK || esp_wifi_scan_get_ap_records(&count, records.get()) != ESP_OK) {
        esp_wifi_clear_ap_list(); wifiNote("扫描失败，确认重试", false); return;
    }
    std::sort(records.get(), records.get() + count, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    withWifi([&](WifiState& s) {
        s.apCount = 0;
        for (unsigned i = 0; i < count; ++i) {
            auto& ap = s.aps[s.apCount++]; const auto& r = records[i];
            snprintf(ap.ssid, sizeof(ap.ssid), "%s", reinterpret_cast<const char*>(r.ssid)); ap.rssi = r.rssi; ap.channel = r.primary; ap.open = r.authmode == WIFI_AUTH_OPEN;
        }
        s.note[0] = 0; s.busy = false; ++s.version;
    });
}
// Five echoes through the WiFi interface; times in ms.
void pingHost(Model::PingRow& row, ip_addr_t target, int netif) {
    struct Run { Model::PingRow* row; uint32_t total; std::atomic<bool> done; } run{&row, 0, {false}};
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target; config.count = 5; config.interval_ms = 300; config.timeout_ms = 1000; config.interface = netif;
    esp_ping_callbacks_t callbacks{}; callbacks.cb_args = &run;
    callbacks.on_ping_success = [](esp_ping_handle_t session, void* args) {
        auto& r = *static_cast<Run*>(args); auto& row = *r.row; uint32_t ms = 0;
        esp_ping_get_profile(session, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
        const uint16_t value = std::min<uint32_t>(ms, 65535);
        if (row.sent == row.lost) row.low = row.high = value;
        row.low = std::min(row.low, value); row.high = std::max(row.high, value); ++row.sent; r.total += value;
        row.average = r.total / (row.sent - row.lost);
    };
    callbacks.on_ping_timeout = [](esp_ping_handle_t, void* args) { auto& row = *static_cast<Run*>(args)->row; ++row.sent; ++row.lost; };
    callbacks.on_ping_end = [](esp_ping_handle_t, void* args) { static_cast<Run*>(args)->done = true; };
    esp_ping_handle_t session;
    if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK) { row.sent = row.lost = 1; return; }
    esp_ping_start(session);
    while (!run.done && !wifiStop) vTaskDelay(pdMS_TO_TICKS(50));
    esp_ping_stop(session); esp_ping_delete_session(session);
}
void pingAll(bool connected) {
    if (!connected) { wifiNote("WiFi 未连接，无法测试延迟", false); return; }
    wifiNote("正在测试延迟…", true);
    esp_netif_ip_info_t info{}; esp_netif_get_ip_info(wifiNetif, &info);
    Model::PingRow rows[3]{}; uint16_t dns = 0;
    strcpy(rows[0].name, "路由器"); snprintf(rows[0].host, sizeof(rows[0].host), IPSTR, IP2STR(&info.gw));
    strcpy(rows[1].name, "阿里 DNS"); strcpy(rows[1].host, "223.5.5.5");
    strcpy(rows[2].name, "百度"); strcpy(rows[2].host, "www.baidu.com");
    const auto show = [&](bool busy) { withWifi([&](WifiState& s) { std::copy(rows, rows + 3, s.pings); s.pingCount = 3; s.dnsMs = dns; s.busy = busy; if (!busy) s.note[0] = 0; ++s.version; }); };
    show(true);
    const int index = esp_netif_get_netif_impl_index(wifiNetif);
    for (unsigned i = 0; i < 3 && !leftTab(1); ++i) {
        ip_addr_t target = IPADDR4_INIT(i == 0 ? info.gw.addr : ipaddr_addr("223.5.5.5"));
        if (i == 2) {
            addrinfo hints{}; hints.ai_family = AF_INET; addrinfo* found = nullptr; const auto at = esp_timer_get_time();
            const bool resolved = !getaddrinfo(rows[2].host, nullptr, &hints, &found) && found;
            dns = std::max<int64_t>(1, (esp_timer_get_time() - at) / 1000);
            if (!resolved) { rows[2].sent = rows[2].lost = 1; strcpy(rows[2].host, "域名解析失败"); show(true); continue; }
            target = IPADDR4_INIT(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr); freeaddrinfo(found);
        }
        pingHost(rows[i], target, index); show(true);
    }
    show(false);
}
// ARP sweep of the /24 around the badge: requests go out a few at a time and the lwIP table (10 entries)
// is read after each batch, before newer replies push older ones out.
struct Sweep { netif* nif; uint32_t base, subnet, mask; unsigned from, to; Model::LanDevice found[32]; unsigned count; };
esp_err_t sweepSend(void* context) {
    auto& w = *static_cast<Sweep*>(context);
    for (unsigned host = w.from; host < w.to; ++host) { ip4_addr_t ip; ip.addr = htonl(w.base | host); etharp_request(w.nif, &ip); }
    return ESP_OK;
}
esp_err_t sweepRead(void* context) {
    auto& w = *static_cast<Sweep*>(context);
    for (size_t i = 0; i < ARP_TABLE_SIZE; ++i) {
        ip4_addr_t* ip; netif* nif; eth_addr* mac;
        if (!etharp_get_entry(i, &ip, &nif, &mac) || nif != w.nif || (ip->addr & w.mask) != w.subnet) continue;
        bool known = false; for (unsigned j = 0; j < w.count; ++j) known |= !memcmp(w.found[j].ip, &ip->addr, 4);
        // The last place is kept for the badge itself.
        if (!known && w.count + 1 < std::size(w.found)) { auto& d = w.found[w.count++]; memcpy(d.ip, &ip->addr, 4); memcpy(d.mac, mac->addr, 6); }
    }
    return ESP_OK;
}
void sweepLan(bool connected) {
    if (!connected) { wifiNote("WiFi 未连接，无法扫描局域网", false); return; }
    auto w = std::unique_ptr<Sweep>(new (std::nothrow) Sweep{});
    if (!w) { wifiNote("内存不足", false); return; }
    esp_netif_ip_info_t info{}; esp_netif_get_ip_info(wifiNetif, &info);
    w->nif = static_cast<netif*>(esp_netif_get_netif_impl(wifiNetif)); w->mask = info.netmask.addr; w->subnet = info.ip.addr & info.netmask.addr;
    w->base = ntohl(info.ip.addr) & 0xffffff00;
    for (unsigned host = 1; host < 255 && !leftTab(2); host += 6) {
        w->from = host; w->to = std::min(host + 6, 255u);
        esp_netif_tcpip_exec(sweepSend, w.get()); vTaskDelay(pdMS_TO_TICKS(100)); esp_netif_tcpip_exec(sweepRead, w.get());
        char text[48]; snprintf(text, sizeof(text), "正在扫描局域网 %u%%  ·  已发现 %u 台", host * 100 / 254, w->count + 1); wifiNote(text, true);
    }
    if (leftTab(2)) return;
    vTaskDelay(pdMS_TO_TICKS(400)); esp_netif_tcpip_exec(sweepRead, w.get());
    // The badge itself answers no ARP request of its own.
    auto& self = w->found[w->count++]; memcpy(self.ip, &info.ip.addr, 4); esp_netif_get_mac(wifiNetif, self.mac);
    std::sort(w->found, w->found + w->count, [](const Model::LanDevice& a, const Model::LanDevice& b) { return a.ip[3] < b.ip[3]; });
    withWifi([&](WifiState& s) {
        s.lanCount = w->count; std::copy(w->found, w->found + w->count, s.lan); s.lanSelf = s.lanGateway = 255;
        for (unsigned i = 0; i < w->count; ++i) { if (!memcmp(w->found[i].ip, &info.ip.addr, 4)) s.lanSelf = i; if (!memcmp(w->found[i].ip, &info.gw.addr, 4)) s.lanGateway = i; }
        s.note[0] = 0; s.busy = false; ++s.version;
    });
}
// Wake-on-LAN: the magic packet (6 x FF, then the MAC 16 times) to UDP port 9, as a subnet and a local broadcast.
void wake(const uint8_t* mac, bool connected) {
    if (!connected) { wifiNote("WiFi 未连接，无法发送唤醒包", false); return; }
    uint8_t packet[102]; memset(packet, 0xff, 6); for (unsigned i = 0; i < 16; ++i) memcpy(packet + 6 + i * 6, mac, 6);
    esp_netif_ip_info_t info{}; esp_netif_get_ip_info(wifiNetif, &info);
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); int yes = 1; bool sent = false;
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(9);
        for (const uint32_t address : {static_cast<uint32_t>(info.ip.addr | ~info.netmask.addr), uint32_t{0xffffffff}}) {
            to.sin_addr.s_addr = address; sent |= sendto(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(packet);
        }
        close(fd);
    }
    withWifi([&](WifiState& s) { s.wakeSentAt = sent ? std::max<time_t>(time(nullptr), 1) : 0; snprintf(s.note, sizeof(s.note), "%s", sent ? "" : "唤醒包发送失败"); s.busy = false; ++s.version; });
}
void wifiRun(void*) {
    uint8_t done = 1 << 3, action = 0; int64_t scannedAt = 0;  // 唤醒 does nothing until Confirm
    withWifi([&](WifiState& s) { action = s.action; });
    while (!wifiStop) {
        uint8_t tab = 0, want = 0, mac[6]{}; bool connected = false;
        withWifi([&](WifiState& s) { tab = s.tab; want = s.action; connected = s.connected; memcpy(mac, s.wakeMac, 6); });
        const bool again = want != action; action = want;
        // The channel chart rescans by itself every 20 s.
        const bool stale = tab == 0 && scannedAt && esp_timer_get_time() - scannedAt > 20000000;
        if (!again && !stale && (done >> tab & 1)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        done |= 1 << tab;
        if (tab == 0) { scanNetworks(); scannedAt = esp_timer_get_time(); }
        else if (tab == 1) pingAll(connected);
        else if (tab == 2) { sweepLan(connected); if (leftTab(2)) done &= ~(1 << 2); }
        else wake(mac, connected);
    }
    wifiRunning = false;
}

// ---- 声音 ----
struct SoundState {
    uint8_t view{}; bool running{}; uint16_t bpm{100};
    uint8_t bars[24]{}; int16_t db{}; uint16_t hz{}; uint8_t beat{}; bool failed{};
};
SoundState soundShared; StaticSemaphore_t soundLockStorage; SemaphoreHandle_t soundLock;
std::atomic<bool> soundStop{}, soundRunning{};
template<class F> void withSound(F f) { xSemaphoreTake(soundLock, portMAX_DELAY); f(soundShared); xSemaphoreGive(soundLock); }
constexpr unsigned kRate = 16000, kFrame = 960, kFft = 512, kBands = 24, kClick = 640;
struct SoundBuffers {
    int16_t frame[kFrame], reference[kFrame];
    int16_t fft[2 * kFft], window[kFft], table[kFft], clicks[2][kClick];
    uint16_t edges[kBands + 1]; float lag[kPitchMaxLag + 1];
};
// 24 bands from 80 Hz to 8 kHz, each the loudest FFT bin in it, as the level of an equal sine in dBFS.
void spectrum(SoundBuffers& b, uint8_t (&bars)[kBands]) {
    const int16_t* x = b.frame + kFrame - kFft; int peak = 1;
    for (unsigned n = 0; n < kFft; ++n) peak = std::max(peak, std::abs(int(x[n])));
    // The fixed-point FFT halves every stage: lift quiet input first, and take the lift off the levels after.
    int shift = 0; while (shift < 8 && peak << (shift + 1) < 32768) ++shift;
    for (unsigned n = 0; n < kFft; ++n) { b.fft[2 * n] = static_cast<int16_t>((x[n] * b.window[n] >> 15) << shift); b.fft[2 * n + 1] = 0; }
    dsps_fft2r_sc16(b.fft, kFft); dsps_bit_rev_sc16_ansi(b.fft, kFft);
    for (unsigned k = 0; k < kBands; ++k) {
        uint32_t loudest = 0;
        for (unsigned bin = b.edges[k]; bin < b.edges[k + 1]; ++bin) {
            const int32_t re = b.fft[2 * bin], im = b.fft[2 * bin + 1];
            loudest = std::max<uint32_t>(loudest, static_cast<uint32_t>(re * re) + static_cast<uint32_t>(im * im));
        }
        // |X| = sine amplitude x 2^shift / 4 after the Hann window and the 1/N scaling.
        const float level = 10 * log10f(loudest + 0.25f) - 6.02f * shift - 78.3f;
        const int bar = std::clamp(static_cast<int>((level + 95) * 100 / 70), 0, 100);
        bars[k] = std::max(bar, bars[k] - 8);  // bars fall back slowly
    }
}
void soundRun(void*) {
    auto b = std::unique_ptr<SoundBuffers>(new (std::nothrow) SoundBuffers{});
    const bool ready = b && board::audioOpen(true, 2, false) && dsps_fft2r_init_sc16(b->table, kFft) == ESP_OK;
    if (ready) {
        for (unsigned n = 0; n < kFft; ++n) b->window[n] = static_cast<int16_t>(32767 * (0.5f - 0.5f * cosf(2 * float(M_PI) * n / (kFft - 1))));
        for (unsigned k = 0; k <= kBands; ++k) {
            const unsigned bin = std::lround(80 * powf(100, float(k) / kBands) / (float(kRate) / kFft));
            b->edges[k] = std::min<unsigned>(kFft / 2, k ? std::max<unsigned>(bin, b->edges[k - 1] + 1) : std::max(bin, 1u));
        }
        // A short decaying tone per beat: higher and louder on the first beat of the bar.
        for (unsigned n = 0; n < kClick; ++n) {
            const float fade = expf(-float(n) / 110);
            b->clicks[0][n] = static_cast<int16_t>(14000 * fade * sinf(2 * float(M_PI) * 1600 * n / kRate));
            b->clicks[1][n] = static_cast<int16_t>(10000 * fade * sinf(2 * float(M_PI) * 1000 * n / kRate));
        }
    }
    withSound([&](SoundState& s) { s.failed = !ready; });
    unsigned position = 0, beat = 0, misses = 0; bool clicking = false; float shown = 0; uint8_t bars[kBands]{}; int smooth = 0;
    while (ready && !soundStop) {
        uint8_t view = 0; bool running = false; unsigned bpm = 100;
        withSound([&](SoundState& s) { view = s.view; running = s.running; bpm = std::clamp<unsigned>(s.bpm, 30, 240); });
        if (view == 2 && running) {
            // The speaker's DMA paces this loop, so beats land on exact sample counts.
            const unsigned length = kRate * 60 / bpm; int16_t block[160];
            if (!clicking) { clicking = true; position = length; beat = 3; }
            for (auto& v : block) {
                if (position >= length) { position = 0; beat = (beat + 1) % 4; withSound([&](SoundState& s) { s.beat = beat; }); }
                v = position < kClick ? b->clicks[beat ? 1 : 0][position] : 0; ++position;
            }
            board::speaker(block, 160, kRate, 1);
            continue;
        }
        if (clicking) { board::silence(); clicking = false; }
        if (!board::microphone(b->frame, b->reference)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int32_t total = 0; for (auto v : b->frame) total += v;
        const int mean = total / int(kFrame); uint64_t energy = 0;
        for (auto& v : b->frame) { v = static_cast<int16_t>(std::clamp(v - mean, -32768, 32767)); energy += int32_t(v) * v; }
        const float rms = sqrtf(float(energy) / kFrame);
        // dB in tenths: dBFS plus 108. An analog MEMS microphone of -38 dBV/Pa behind the codec's 24 dB gain puts
        // 94 dB SPL at -14 dBFS; the page adds its calibration.
        const int db = rms > 0.5f ? std::lround((20 * log10f(rms / 32768) + 108) * 10) : 0;
        smooth = smooth ? (smooth * 2 + db) / 3 : db;
        if (view == 0) spectrum(*b, bars);
        if (view == 1) {
            static_assert(kPitchWindow + kPitchMaxLag <= kFrame);
            const float hz = rms > 100 ? pitchHz(b->frame, b->lag, kRate) : 0;
            if (hz >= 40 && hz <= 1000) { shown = hz; misses = 0; } else if (++misses > 5) shown = 0;  // hold a moment between notes
        }
        withSound([&](SoundState& s) { memcpy(s.bars, bars, sizeof(bars)); s.db = static_cast<int16_t>(std::max(smooth, 1)); s.hz = static_cast<uint16_t>(std::lround(shown * 10)); });
    }
    if (clicking) board::silence();
    dsps_fft2r_deinit_sc16();
    if (ready) board::audioClose();
    soundRunning = false;
}

// ---- 白噪音 and the alarm's ring ----
// kind: a NoiseGen sound, kRing for the alarm, kQuiet to fade out and end.
constexpr uint8_t kRing = 100, kQuiet = 255;
std::atomic<uint8_t> noiseKind{kQuiet}; std::atomic<bool> noiseRunning{};
void noiseRun(void*) {
    NoiseGen noise; AlarmTone ring; int16_t block[160]; uint8_t playing = noiseKind; bool loud = false; unsigned failures = 0;
    if (board::audioOpen(false)) {
        noise.start();
        while (true) {
            const uint8_t want = noiseKind;
            // The ring stops at once; a sound fades out, still playing its kind meanwhile.
            if (want == kQuiet && playing == kRing) break;
            if (want == kQuiet) noise.stop();
            else { if (want != playing) { noise.start(); if (want == kRing) ring.reset(); } playing = want; }
            // A ring is loud whatever the volume, without saving that: the next audioOpen sets the saved level.
            if ((playing == kRing) != loud) { loud = playing == kRing; board::outputVolume(loud ? std::max(board::volume(), 80u) : board::volume()); }
            if (playing == kRing) ring.fill(block, 160);
            else { noise.fill(playing, block, 160); if (noise.silent()) break; }
            // DMA paces the loop. A speaker that refuses the block would leave it spinning, starving every lower task
            // (the main loop, so USB never came up while a reminder rang at boot): wait, and give up after a second.
            if (board::speaker(block, 160, NoiseGen::kRate, 1)) failures = 0;
            else if (++failures > 100) break;
            else vTaskDelay(pdMS_TO_TICKS(10));
        }
        board::silence(); board::audioClose();
    }
    noiseRunning = false;
}

// ---- 语音唤醒 ----
// ESP-SR WakeNet9s "你好小智". Its weights stay in flash: srmodels.bin is written to the free region after the identity
// card (flash_passport_device.py) and registered here as the partition "model" esp_srmodel_init() looks for, so the
// shipped partition table stays as it is. Its working memory exists only while this task runs.
constexpr uint32_t kModelAt = 0x670000, kModelBytes = 0x40000;
std::atomic<bool> wakeStop{}, wakeRunning{}, wakeHeard{};
std::atomic<unsigned> wakeState{}, wakeError{}, wakeHeap{}, wakeLargest{}, wakeChunk{}, wakeFrames{}, wakeDetects{}, wakeUs{}, wakeStack{}, wakeAfterHeap{}, wakeAfterLargest{};
srmodel_list_t* wakeModels() {
    static srmodel_list_t* models = [] () -> srmodel_list_t* {
        const esp_partition_t* part = nullptr;
        if (esp_partition_register_external(nullptr, kModelAt, kModelBytes, "model", ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_DATA_SPIFFS, &part) != ESP_OK) return nullptr;
        // srmodel_load() trusts what it reads: check the packed header (one model, ours) before handing it over.
        char head[36]{}; static constexpr char expected[36] = "\x01\0\0\0wn9s_nihaoxiaozhi";
        if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK || memcmp(head, expected, sizeof(head))) return nullptr;
        return esp_srmodel_init("model");
    }();
    return models;
}
void wakeRun(void*) {
    // 1 = model missing from flash, 2 = out of memory, 3 = microphone failed.
    auto* models = wakeModels(); char* name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi") : nullptr;
    auto* wn = name ? reinterpret_cast<const esp_wn_iface_t*>(esp_wn_handle_from_name(name)) : nullptr;
    model_iface_data_t* data = wn ? wn->create(name, DET_MODE_90) : nullptr;
    wakeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT); wakeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const unsigned chunk = data ? wn->get_samp_chunksize(data) : 0; wakeChunk = chunk;
    auto samples = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[2 * kFrame + chunk]);  // frame, reference, chunk
    const bool ready = data && samples && chunk && chunk <= kFrame && board::audioOpen(true, 2, false);
    wakeError = !name ? 1 : !ready && (!data || !samples) ? 2 : !ready ? 3 : 0; wakeState = ready ? 1 : 2;
    unsigned filled = 0, frames = 0; uint64_t busy = 0;
    while (ready && !wakeStop && !wakeHeard) {
        int16_t* frame = samples.get(); int16_t* pending = frame + 2 * kFrame;
        if (!board::microphone(frame, frame + kFrame)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        // The model eats chunks of its own size; carry the rest of each 60 ms frame over.
        for (unsigned at = 0; at < kFrame && !wakeHeard;) {
            const unsigned take = std::min(chunk - filled, kFrame - at);
            memcpy(pending + filled, frame + at, take * sizeof(int16_t)); filled += take; at += take;
            if (filled < chunk) continue;
            filled = 0; const auto started = esp_timer_get_time();
            if (wn->detect(data, pending) > 0) { wakeHeard = true; ++wakeDetects; }
            busy += esp_timer_get_time() - started; ++frames;
        }
        wakeFrames = frames; wakeUs = frames ? busy / frames : 0; wakeStack = uxTaskGetStackHighWaterMark(nullptr);
    }
    if (ready) board::audioClose();
    if (data) wn->destroy(data);
    wakeState = 0; wakeRunning = false;
}

// ---- 对讲机 ----
// Push to talk over UDP broadcast on the WiFi subnet: each packet is one 60 ms Opus frame (16 kHz mono, 16 kbps CBR, so
// 120 bytes) after a 28-byte header; a talk ends with three copies of an end packet. scripts/walkie_mac.py speaks it too.
// The receiver keeps only the channel's current talk (the first talker wins until 1 s of silence) and queues its frames;
// the page's audio task plays them a few frames late, or records and sends while 说话 is on (then nothing is heard).
constexpr uint16_t kWalkiePort = 7310;
constexpr unsigned kWalkieOpus = 160, kWalkieSlots = 16, kWalkieHeadStart = 3;
struct WalkiePacket {
    char magic[2]; uint8_t version, channel, sender[3], flags; uint16_t talk, seq;  // flags 1: the talk ended
    char name[16];                                                                // the sender's card name, UTF-8
    uint8_t opus[kWalkieOpus];
};
static_assert(offsetof(WalkiePacket, name) == 12 && offsetof(WalkiePacket, opus) == 28);
struct WalkieFrame { uint16_t seq; uint8_t size; bool end; uint8_t opus[kWalkieOpus]; };
struct WalkieQueue { WalkieFrame frames[kWalkieSlots]; unsigned head, count; };
StaticSemaphore_t walkieLockStorage; SemaphoreHandle_t walkieLock;
WalkieQueue* walkieQueue;  // the receiver's, while it runs
char walkieFrom[16], walkieSelfName[16]; uint8_t walkieSelf[3];
std::atomic<uint8_t> walkieChannel{1}, walkiePhase{};  // phase as Model::walkiePhase
std::atomic<bool> walkieRxStop{}, walkieRxRunning{}, walkieStop{}, walkieRunning{}, walkieWant{}, walkieTalking{};
std::atomic<unsigned> walkieCalls{}, walkieIn{}, walkiePlayed{}, walkieSent{}, walkieErrors{}, walkieStack{}, walkieHeap{};
std::atomic<int> walkieLastError{};  // the last codec or socket error, for /status
std::atomic<uint32_t> walkieBroadcast{};  // the WiFi subnet's broadcast address, 0 while unknown
// Copies at most 15 bytes of a name, cut between UTF-8 characters.
void walkieName(char (&to)[16], const char* from, size_t size) {
    const bool cut = size > sizeof(to) - 1; size = std::min<size_t>(size, sizeof(to) - 1);
    if (cut) while (size && (static_cast<uint8_t>(from[size]) & 0xc0) == 0x80) --size;
    memcpy(to, from, size); to[size] = 0;
}
template<class F> void withWalkie(F f) { xSemaphoreTake(walkieLock, portMAX_DELAY); f(); xSemaphoreGive(walkieLock); }
void walkieListen(void*) {
    auto queue = std::unique_ptr<WalkieQueue>(new (std::nothrow) WalkieQueue{});
    const int fd = queue ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
    sockaddr_in at{}; at.sin_family = AF_INET; at.sin_port = htons(kWalkiePort); at.sin_addr.s_addr = htonl(INADDR_ANY);
    const timeval wait{0, 200000};  // so a stop is seen within 0.2 s
    if (fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) == 0) {
        withWalkie([&] { walkieQueue = queue.get(); });
        WalkiePacket p; uint32_t current = 0; uint16_t last = 0; int64_t currentAt = 0;  // current: sender << 16 | talk + 1
        while (!walkieRxStop) {
            const int n = recv(fd, &p, sizeof(p), 0);
            if (n < int(offsetof(WalkiePacket, opus)) || memcmp(p.magic, "PT", 2) || p.version != 1 || p.channel != walkieChannel || !memcmp(p.sender, walkieSelf, 3)) continue;
            ++walkieIn;
            if (walkieTalking) { current = 0; continue; }
            const uint32_t key = ((uint32_t(p.sender[0]) << 16 | p.sender[1] << 8 | p.sender[2]) << 8 ^ p.talk) + 1;
            const int64_t now = esp_timer_get_time(); const bool end = p.flags & 1;
            if (key != current) {
                if ((current && now - currentAt < 1000000) || end) continue;  // someone else holds the channel, or a stray end
                current = key; last = p.seq - 1; ++walkieCalls;
                withWalkie([&] { walkieName(walkieFrom, p.name, strnlen(p.name, sizeof(p.name))); });
            }
            if (int16_t(p.seq - last) <= 0) continue;  // a copy, or late
            last = p.seq; currentAt = now; if (end) current = 0;
            withWalkie([&] {
                auto& q = *walkieQueue;
                if (q.count == kWalkieSlots) { q.head = (q.head + 1) % kWalkieSlots; --q.count; }  // full: the oldest goes
                auto& f = q.frames[(q.head + q.count++) % kWalkieSlots];
                f.seq = p.seq; f.end = end; f.size = end ? 0 : std::min<unsigned>(n - offsetof(WalkiePacket, opus), kWalkieOpus); memcpy(f.opus, p.opus, f.size);
            });
        }
        withWalkie([&] { walkieQueue = nullptr; });
    }
    if (fd >= 0) close(fd);
    walkieRxRunning = false;
}
// Frames queued, and the oldest taken out (false: none).
unsigned walkieQueued() { unsigned n = 0; withWalkie([&] { n = walkieQueue ? walkieQueue->count : 0; }); return n; }
bool walkieTake(WalkieFrame& f) {
    bool got = false;
    withWalkie([&] { if (auto* q = walkieQueue; q && q->count) { f = q->frames[q->head]; q->head = (q->head + 1) % kWalkieSlots; --q->count; got = true; } });
    return got;
}
void walkieClear() { withWalkie([&] { if (walkieQueue) walkieQueue->count = 0; }); }
struct WalkieBuffers { int16_t pcm[kFrame], reference[kFrame]; WalkiePacket out; WalkieFrame in; };
void walkieRun(void*) {
    auto b = std::unique_ptr<WalkieBuffers>(new (std::nothrow) WalkieBuffers{});
    const bool ready = b && board::audioOpen(true, 2, false);
    walkiePhase = ready ? 0 : 3;
    void *encoder = nullptr, *decoder = nullptr; int fd = -1;
    std::unique_ptr<uint8_t[]> encoded; int encodedSize = 0;  // the encoder wants its own recommended output size
    uint16_t talk = static_cast<uint16_t>(esp_random()), seq = 0, expected = 0; bool playing = false; int64_t waiting = 0, heard = 0;
    // bytes 0: the end packet.
    const auto send = [&](size_t bytes) {
        auto& p = b->out; memcpy(p.magic, "PT", 2); p.version = 1; p.channel = walkieChannel; memcpy(p.sender, walkieSelf, 3);
        p.flags = !bytes; p.talk = talk; p.seq = seq++; memcpy(p.name, walkieSelfName, sizeof(p.name));
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(kWalkiePort); to.sin_addr.s_addr = walkieBroadcast;
        return sendto(fd, &p, offsetof(WalkiePacket, opus) + bytes, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) > 0;
    };
    const auto stopPlaying = [&] {
        if (!playing) return;
        memset(b->pcm, 0, sizeof(b->pcm)); board::speaker(b->pcm, 640, 16000, 1);  // let the last frame leave the DMA
        board::silence(); esp_opus_dec_close(decoder); decoder = nullptr; playing = false; walkiePhase = 0;
    };
    while (ready && !walkieStop) {
        if (walkieWant) {
            // Talking: one microphone frame in, one packet out, every 60 ms.
            if (!encoder) {
                stopPlaying();
                esp_opus_enc_config_t c = ESP_OPUS_ENC_CONFIG_DEFAULT(); c.sample_rate = 16000; c.channel = 1; c.bitrate = 16000;
                c.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
                fd = walkieBroadcast ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1; int yes = 1;
                int inSize = 0;
                if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) || esp_opus_enc_open(&c, sizeof(c), &encoder) != ESP_AUDIO_ERR_OK
                    || esp_opus_enc_get_frame_size(encoder, &inSize, &encodedSize) != ESP_AUDIO_ERR_OK || inSize != int(sizeof(b->pcm))
                    || !(encoded = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[encodedSize]))) {
                    if (fd >= 0) close(fd);
                    if (encoder) esp_opus_enc_close(encoder);
                    fd = -1; encoder = nullptr; ++walkieErrors; walkieLastError = -100 - inSize; walkieWant = false; walkiePhase = 3; continue;
                }
                walkieHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                ++talk; seq = 0; walkieTalking = true; walkieClear(); walkiePhase = 1;
                for (unsigned i = 0; i < 20 && board::microphone(b->pcm, b->reference); ++i) {}  // what came before Confirm
            }
            if (!board::microphone(b->pcm, b->reference)) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            esp_audio_enc_in_frame_t in{}; in.buffer = reinterpret_cast<uint8_t*>(b->pcm); in.len = sizeof(b->pcm);
            esp_audio_enc_out_frame_t out{}; out.buffer = encoded.get(); out.len = encodedSize;
            const auto rc = esp_opus_enc_process(encoder, &in, &out);
            if (rc != ESP_AUDIO_ERR_OK || !out.encoded_bytes || out.encoded_bytes > kWalkieOpus) { ++walkieErrors; walkieLastError = rc ? rc : -int(out.encoded_bytes); continue; }
            memcpy(b->out.opus, encoded.get(), out.encoded_bytes);
            if (send(out.encoded_bytes)) ++walkieSent; else { ++walkieErrors; walkieLastError = -1000 - errno; }
            walkieStack = uxTaskGetStackHighWaterMark(nullptr);
            continue;
        }
        if (encoder) {
            const uint16_t end = seq;
            for (unsigned i = 0; i < 3; ++i) { seq = end; send(0); vTaskDelay(pdMS_TO_TICKS(20)); }  // one end packet, three times
            esp_opus_enc_close(encoder); encoder = nullptr; encoded.reset(); close(fd); fd = -1; walkieTalking = false; walkiePhase = 0;
        }
        // Hearing: start a few frames behind, so a late packet still finds the speaker going.
        const int64_t now = esp_timer_get_time();
        if (!playing) {
            const unsigned queued = walkieQueued();
            if (!queued) { waiting = 0; board::discardMicrophone(); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (!waiting) waiting = now;
            if (queued < kWalkieHeadStart && now - waiting < 200000) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            esp_opus_dec_cfg_t c = ESP_OPUS_DEC_CONFIG_DEFAULT(); c.sample_rate = 16000; c.channel = 1;
            if (esp_opus_dec_open(&c, sizeof(c), &decoder) != ESP_AUDIO_ERR_OK) { decoder = nullptr; ++walkieErrors; walkieClear(); walkiePhase = 3; continue; }
            walkieHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            playing = true; waiting = 0; heard = now; expected = 0; walkiePhase = 2;
        }
        board::discardMicrophone();
        auto& f = b->in;
        if (!walkieTake(f)) {
            // Nothing yet: the speaker plays silence meanwhile (its DMA callback clears what it played); 0.6 s without
            // a frame ends the talk.
            if (now - heard > 600000) { stopPlaying(); continue; }
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (f.end) { stopPlaying(); continue; }
        heard = now;
        if (walkieQueued() > 8) { expected = f.seq + 1; continue; }  // fallen behind: skip a frame to catch up
        if (expected && f.seq != expected) { memset(b->pcm, 0, sizeof(b->pcm)); for (unsigned i = 0; i < std::min<unsigned>(uint16_t(f.seq - expected), 2); ++i) board::speaker(b->pcm, kFrame, 16000, 1); }
        expected = f.seq + 1;
        esp_audio_dec_in_raw_t raw{}; raw.buffer = f.opus; raw.len = f.size;
        esp_audio_dec_out_frame_t out{}; out.buffer = reinterpret_cast<uint8_t*>(b->pcm); out.len = sizeof(b->pcm); esp_audio_dec_info_t info{};
        const auto rc = esp_opus_dec_decode(decoder, &raw, &out, &info);
        if (rc == ESP_AUDIO_ERR_OK && board::speaker(b->pcm, out.decoded_size / 2, 16000, 1)) ++walkiePlayed; else { ++walkieErrors; walkieLastError = rc ? -2000 + rc : -3000; }
        walkieStack = uxTaskGetStackHighWaterMark(nullptr);
    }
    stopPlaying();
    if (encoder) { esp_opus_enc_close(encoder); close(fd); }
    walkieTalking = false;
    if (ready) board::audioClose();
    walkieRunning = false;
}

// ---- 移动监控 ----
// CSI is the channel's amplitude and phase on each OFDM subcarrier of a received frame. A body moving in the room
// changes the paths the router's signal takes to the badge, so the amplitudes start to wander from frame to frame.
// A ping to the router every 20 ms keeps its frames coming: its beacons carry no CSI here, and other access
// points are not heard on the router's channel (measured: 20 frames/s from the router at a 50 ms ping, 0.1 from the rest).
constexpr unsigned kCarriers = 51;  // L-LTF subcarriers 2..26 and -26..-1: buffer slots 2..26 and 38..63
struct MotionCsi {
    uint8_t router[6]; bool primed;
    float mean[kCarriers], spread[kCarriers];  // per subcarrier, averaged over the last ~8 frames
    float score; uint32_t packets; int32_t rssi;  // summed per frame, taken twice a second
};
portMUX_TYPE motionMux = portMUX_INITIALIZER_UNLOCKED;
// Runs on the WiFi task for every frame with CSI.
void csiReceived(void* context, wifi_csi_info_t* info) {
    auto& c = *static_cast<MotionCsi*>(context);
    if (!info || info->len < 128 || memcmp(info->mac, c.router, 6)) return;
    float a[kCarriers], total = 0; unsigned n = 0;
    for (unsigned slot = 2; slot < 64; ++slot) {
        if (slot > 26 && slot < 38) continue;  // DC is slot 0; 27..37 are the guard band
        const float im = info->buf[2 * slot], re = info->buf[2 * slot + 1];
        total += a[n++] = sqrtf(re * re + im * im);
    }
    if (total < 1) return;
    // Divided by their mean, the amplitudes no longer move with the receiver's gain control.
    const float scale = kCarriers / total; float wander = 0;
    for (unsigned k = 0; k < kCarriers; ++k) {
        const float x = a[k] * scale;
        if (!c.primed) { c.mean[k] = x; c.spread[k] = 0; continue; }
        const float d = x - c.mean[k];
        c.mean[k] += d / 8; c.spread[k] += (d * d - c.spread[k]) / 8; wander += c.spread[k];
    }
    taskENTER_CRITICAL(&motionMux);
    if (c.primed) { c.score += wander / kCarriers; ++c.packets; c.rssi += info->rx_ctrl.rssi; }
    c.primed = true;
    taskEXIT_CRITICAL(&motionMux);
}
struct MotionState {
    bool connected{}; uint8_t alarm{1}, limit{50}, calibrate{};  // from the page
    uint8_t phase{}, level{}, calLeft{}, rate{}; int8_t rssi{}; uint8_t history[60]{}, count{}; uint16_t events{}; uint32_t lastAt{}, version{};
};
MotionState* motionShared; StaticSemaphore_t motionLockStorage; SemaphoreHandle_t motionLock;
std::atomic<bool> motionStop{}, motionRunning{}; esp_netif_t* motionNetif;
template<class F> void withMotion(F f) { xSemaphoreTake(motionLock, portMAX_DELAY); f(*motionShared); xSemaphoreGive(motionLock); }
constexpr unsigned kCalibration = 10;  // half-second samples: 5 s of quiet
// Three short 2 kHz beeps at the system volume; 2 kHz is exactly 8 samples at 16 kHz.
void motionBeep() {
    if (!board::audioOpen(false)) return;
    int16_t tone[160], quiet[160]{};
    for (unsigned i = 0; i < 160; ++i) tone[i] = static_cast<int16_t>(12000 * sinf(2 * float(M_PI) * (i % 8) / 8));
    for (unsigned beep = 0; beep < 3; ++beep) {
        for (unsigned block = 0; block < 12; ++block) board::speaker(tone, 160, 16000, 1);   // 120 ms
        for (unsigned block = 0; block < 8; ++block) board::speaker(quiet, 160, 16000, 1);   // 80 ms
    }
    board::silence(); board::audioClose();
}
void motionRun(void*) {
    auto csi = std::unique_ptr<MotionCsi>(new (std::nothrow) MotionCsi{});
    esp_ping_handle_t ping = nullptr; bool listening = false;
    float baseline = 0, quiet[kCalibration]; unsigned calibrated = 0, above = 0, below = 0, silent = 0; bool moving = false; int64_t beepedAt = 0;
    uint8_t calibrate = 0; withMotion([&](MotionState& s) { calibrate = s.calibrate; });
    const auto stopListening = [&] {
        if (ping) { esp_ping_stop(ping); esp_ping_delete_session(ping); ping = nullptr; }
        if (listening) { esp_wifi_set_csi(false); esp_wifi_set_csi_rx_cb(nullptr, nullptr); listening = false; vTaskDelay(pdMS_TO_TICKS(100)); }
    };
    const auto publish = [&](uint8_t phase, int level) {
        withMotion([&](MotionState& s) {
            s.phase = phase; s.level = static_cast<uint8_t>(level); s.calLeft = static_cast<uint8_t>((kCalibration - std::min(calibrated, kCalibration) + 1) / 2);
            if (phase >= 2 && phase <= 3) {
                if (s.count == std::size(s.history)) memmove(s.history, s.history + 1, --s.count);
                s.history[s.count++] = static_cast<uint8_t>(level);
            }
            ++s.version;
        });
    };
    while (!motionStop) {
        vTaskDelay(pdMS_TO_TICKS(500));
        bool connected = false; uint8_t alarm = 1, limit = 50, want = 0;
        withMotion([&](MotionState& s) { connected = s.connected; alarm = s.alarm; limit = s.limit; want = s.calibrate; });
        wifi_ap_record_t ap{};
        if (!csi || !connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { stopListening(); calibrated = 0; moving = false; publish(csi ? 0 : 4, 0); continue; }
        if (!listening) {
            memcpy(csi->router, ap.bssid, 6); csi->primed = false;
            wifi_csi_config_t config{}; config.lltf_en = true;  // HT-LTF off: every frame gives the same 64 L-LTF subcarriers
            listening = esp_wifi_set_csi_config(&config) == ESP_OK && esp_wifi_set_csi_rx_cb(csiReceived, csi.get()) == ESP_OK && esp_wifi_set_csi(true) == ESP_OK;
            esp_netif_ip_info_t info{}; esp_netif_get_ip_info(motionNetif, &info);
            esp_ping_config_t pingConfig = ESP_PING_DEFAULT_CONFIG();
            pingConfig.target_addr = IPADDR4_INIT(info.gw.addr); pingConfig.count = ESP_PING_COUNT_INFINITE; pingConfig.interval_ms = 20; pingConfig.timeout_ms = 1000;
            pingConfig.data_size = 16; pingConfig.interface = esp_netif_get_netif_impl_index(motionNetif);
            esp_ping_callbacks_t callbacks{};
            if (listening && esp_ping_new_session(&pingConfig, &callbacks, &ping) == ESP_OK) esp_ping_start(ping);
            taskENTER_CRITICAL(&motionMux); csi->score = 0; csi->packets = 0; csi->rssi = 0; taskEXIT_CRITICAL(&motionMux);
            continue;
        }
        if (want != calibrate) { calibrate = want; calibrated = 0; moving = false; above = below = 0; }
        taskENTER_CRITICAL(&motionMux);
        const float total = csi->score; const uint32_t packets = csi->packets; const int32_t rssi = csi->rssi;
        csi->score = 0; csi->packets = 0; csi->rssi = 0;
        taskEXIT_CRITICAL(&motionMux);
        withMotion([&](MotionState& s) { s.rate = static_cast<uint8_t>(std::min<uint32_t>(packets * 2, 255)); if (packets) s.rssi = static_cast<int8_t>(rssi / int32_t(packets)); });
        if (!packets) { if (++silent >= 6) publish(4, 0); continue; }
        silent = 0;
        const float score = total / packets;
        if (calibrated < kCalibration) {
            quiet[calibrated++] = score;
            // The middle of the quiet samples, so one bump while calibrating does not set the bar.
            if (calibrated == kCalibration) { std::sort(quiet, quiet + kCalibration); baseline = std::max(quiet[kCalibration / 2], 1e-7f); }
            publish(1, 0); continue;
        }
        const int level = std::clamp(static_cast<int>(std::lround(10 + 20 * log2f(score / baseline))), 0, 100);
        if (level > limit) { ++above; below = 0; } else { ++below; above = 0; }
        // Movement: over the line for 1 s; over: under it for 3 s.
        const bool started = !moving && above >= 2;
        if (started) moving = true;
        if (moving && below >= 6) moving = false;
        // A quiet room drifts (a door left open, someone sitting still); the baseline follows it over half a minute.
        if (!moving && level <= limit) baseline += (score - baseline) / 64;
        if (moving) withMotion([&](MotionState& s) { s.lastAt = std::max<time_t>(time(nullptr), 1); s.events += started; });
        publish(moving ? 3 : 2, level);
        // Once: as a movement starts. Continuous: then every 3 s for as long as it lasts.
        if (alarm && (started || (alarm == 2 && moving && esp_timer_get_time() - beepedAt >= 3000000))) { motionBeep(); beepedAt = esp_timer_get_time(); }
    }
    stopListening();
    motionRunning = false;
}
}

bool wifiTick(Runtime& runtime, esp_netif_t* station, bool connected) {
    if (!wifiLock) wifiLock = xSemaphoreCreateMutexStatic(&wifiLockStorage);
    auto& m = runtime.model; static uint32_t seen;
    if (runtime.active() != App::Wifi) {
        if (wifiRunning) wifiStop = true;
        else { delete wifiShared; wifiShared = nullptr; }
        return false;
    }
    if (wifiRunning && wifiStop) return false;  // the last visit's task is still finishing a job
    if (!wifiRunning) {
        if (!wifiShared && !(wifiShared = new (std::nothrow) WifiState{})) { snprintf(m.wifiNote, sizeof(m.wifiNote), "内存不足"); return true; }
        withWifi([&](WifiState& s) { s = WifiState{}; s.action = m.wifiAction; s.version = 1; });
        wifiNetif = station; wifiStop = false; wifiRunning = true; seen = 0;
        if (xTaskCreate(taskMain<wifiRun>, "wifi_tools", 4096, nullptr, 1, nullptr) != pdPASS) { wifiRunning = false; snprintf(m.wifiNote, sizeof(m.wifiNote), "内存不足"); return true; }
    }
    bool changed = false;
    withWifi([&](WifiState& s) {
        s.tab = m.wifiTab; s.action = m.wifiAction; s.connected = connected; memcpy(s.wakeMac, m.tools.wakeMac, 6);
        if (s.version == seen) return;
        seen = s.version; changed = true;
        m.wifiApCount = s.apCount; std::copy(s.aps, s.aps + s.apCount, m.page.wifi.aps.begin());
        m.pingCount = s.pingCount; std::copy(s.pings, s.pings + s.pingCount, m.page.wifi.pings.begin()); m.dnsMs = s.dnsMs;
        m.lanCount = s.lanCount; std::copy(s.lan, s.lan + s.lanCount, m.page.wifi.lan.begin()); m.lanSelf = s.lanSelf; m.lanGateway = s.lanGateway;
        snprintf(m.wifiNote, sizeof(m.wifiNote), "%s", s.note); m.wifiBusy = s.busy; if (s.wakeSentAt) m.wakeSentAt = s.wakeSentAt;
    });
    return changed;
}
bool motionTick(Runtime& runtime, esp_netif_t* station, bool connected) {
    if (!motionLock) motionLock = xSemaphoreCreateMutexStatic(&motionLockStorage);
    auto& m = runtime.model; static uint32_t seen;
    if (runtime.active() != App::Motion) {
        if (motionRunning) motionStop = true;
        else { delete motionShared; motionShared = nullptr; }
        return false;
    }
    if (motionRunning && motionStop) return false;
    if (!motionRunning) {
        if (!motionShared && !(motionShared = new (std::nothrow) MotionState{})) { m.motionPhase = 4; return true; }
        withMotion([&](MotionState& s) { s = MotionState{}; s.calibrate = m.motionCalibrate; s.version = 1; });
        motionNetif = station; motionStop = false; motionRunning = true; seen = 0;
        if (xTaskCreate(taskMain<motionRun>, "motion", 3072, nullptr, 1, nullptr) != pdPASS) { motionRunning = false; m.motionPhase = 4; return true; }
    }
    bool changed = false; const uint16_t events = m.motionEvents;
    withMotion([&](MotionState& s) {
        s.connected = connected; s.alarm = m.tools.alarm(); s.limit = m.tools.motionLimit(); s.calibrate = m.motionCalibrate;
        if (s.version == seen) return;
        seen = s.version; changed = true;
        m.motionPhase = s.phase; m.motionLevel = s.level; m.motionCalLeft = s.calLeft; m.motionRate = s.rate; m.motionRssi = s.rssi;
        m.motionHistoryCount = s.count; std::copy(s.history, s.history + s.count, m.motionHistory.begin()); m.motionEvents = s.events; m.motionLastAt = s.lastAt;
    });
    // Movement lights the screen up.
    if (m.motionEvents != events) runtime.wake(esp_timer_get_time() / 1000000);
    return changed;
}
bool soundTick(Runtime& runtime, bool awake) {
    if (!soundLock) soundLock = xSemaphoreCreateMutexStatic(&soundLockStorage);
    auto& m = runtime.model;
    if (runtime.active() != App::Sound || !awake) {
        // Stop before anyone else opens the codec: the task closes it on its way out.
        if (soundRunning) { soundStop = true; for (unsigned i = 0; i < 50 && soundRunning; ++i) vTaskDelay(pdMS_TO_TICKS(10)); }
        return false;
    }
    if (soundRunning && soundStop) return false;
    if (!soundRunning) {
        withSound([&](SoundState& s) { s = SoundState{}; });
        soundStop = false; soundRunning = true;
        if (xTaskCreate(taskMain<soundRun>, "sound", 4096, nullptr, 1, nullptr) != pdPASS) { soundRunning = false; m.soundFailed = true; return true; }
    }
    withSound([&](SoundState& s) {
        s.view = m.soundView; s.running = m.soundRunning; s.bpm = m.tools.bpm;
        m.soundFailed = s.failed; std::copy(s.bars, s.bars + kBands, m.soundBars.begin()); m.soundHz = s.hz; m.soundBeat = s.beat;
        m.soundDb = s.db ? static_cast<int16_t>(std::max(1, s.db + m.tools.soundCal * 10)) : 0;
    });
    m.soundMax = std::max(m.soundMax, m.soundDb);
    return false;
}
bool noiseTick(Runtime& runtime, bool codecFree) {
    const auto& m = runtime.model; const auto active = runtime.active();
    const uint8_t want = (active == App::Alarm && m.alarmRinging) || (active == App::Todo && m.todoRinging) ? kRing : active == App::Noise && m.noisePlaying ? m.noiseKind : kQuiet;
    noiseKind = want;
    if (want == kQuiet) {
        // Leaving the page: wait for the fade (0.5 s) so the next page finds the codec closed.
        if (noiseRunning && active != App::Noise && active != App::Alarm && active != App::Todo) for (unsigned i = 0; i < 80 && noiseRunning; ++i) vTaskDelay(pdMS_TO_TICKS(10));
        return false;
    }
    if (noiseRunning || !codecFree) return false;
    noiseRunning = true;
    if (xTaskCreate(taskMain<noiseRun>, "noise", 3072, nullptr, 2, nullptr) != pdPASS) noiseRunning = false;
    return false;
}
bool wakeTick(Runtime& runtime, bool listen) {
    if (!listen || wakeHeard) {
        // Stop before anyone else opens the codec: the task closes it on its way out.
        if (wakeRunning) { wakeStop = true; for (unsigned i = 0; i < 100 && wakeRunning; ++i) vTaskDelay(pdMS_TO_TICKS(10)); }
        if (!wakeHeard || wakeRunning) return false;
        wakeHeard = false;
        wakeAfterHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT); wakeAfterLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (runtime.active() != App::Home && runtime.active() != App::Menu) return false;
        const auto now = esp_timer_get_time() / 1000000;
        // 语音指令, not 小智语音: a command never reaches the chat's history or memory.
        runtime.wake(now); runtime.switchTo(App::Voice); return true;
    }
    // A model that failed to load stays failed until the next boot; memory may free up, so retry that after a pause.
    static int64_t retryAt = 0;
    if (wakeRunning || wakeError == 1 || esp_timer_get_time() < retryAt) return false;
    if (wakeError) retryAt = esp_timer_get_time() + 10000000;
    wakeStop = false; wakeRunning = true; wakeState = 1;
    if (xTaskCreate(taskMain<wakeRun>, "wake", 4096, nullptr, 2, nullptr) != pdPASS) { wakeRunning = false; wakeState = 2; wakeError = 2; retryAt = esp_timer_get_time() + 10000000; }
    return false;
}
bool walkieTick(Runtime& runtime, esp_netif_t* station, bool connected, bool codecFree) {
    if (!walkieLock) {
        walkieLock = xSemaphoreCreateMutexStatic(&walkieLockStorage);
        uint8_t mac[6]{}; esp_read_mac(mac, ESP_MAC_WIFI_STA); memcpy(walkieSelf, mac + 3, 3);
    }
    auto& m = runtime.model; const bool page = runtime.active() == App::Walkie; const auto now = esp_timer_get_time() / 1000000;
    walkieChannel = m.walkieChannel;
    esp_netif_ip_info_t info{};
    walkieBroadcast = connected && esp_netif_get_ip_info(station, &info) == ESP_OK && info.ip.addr ? info.ip.addr | ~info.netmask.addr : 0;
    // The receiver: a small task and a socket, on while it is wanted and WiFi is up.
    if (!(connected && (page || m.walkieListen))) walkieRxStop = true;
    else if (!walkieRxRunning) {
        walkieRxStop = false; walkieRxRunning = true;
        if (xTaskCreate(taskMain<walkieListen>, "walkie_rx", 3072, nullptr, 3, nullptr) != pdPASS) walkieRxRunning = false;
    }
    // A new call heard on another page opens the page (the page it interrupts closes first).
    static unsigned calls = 0; const unsigned heard = walkieCalls;
    if (heard != calls) { calls = heard; if (!page && m.walkieListen) { runtime.walkieCall(now); return true; } }
    if (!page) {
        // Stop before anyone else opens the codec: the task ends a talk (its end packets) and closes it on its way out.
        if (walkieRunning) { walkieWant = false; walkieStop = true; for (unsigned i = 0; i < 150 && walkieRunning; ++i) vTaskDelay(pdMS_TO_TICKS(10)); }
        if (!walkieRunning) walkiePhase = 0;
        return false;
    }
    bool changed = false;
    if (const uint8_t phase = walkiePhase; phase != m.walkiePhase) {
        if (phase == 3) { m.walkieTalk = false; walkieWant = false; }
        if (phase == 2) { withWalkie([&] { memcpy(m.walkieFrom, walkieFrom, sizeof(m.walkieFrom)); }); runtime.wake(now); }
        if (phase != 1) m.walkieAt = now;  // talking counts from Confirm
        m.walkiePhase = phase; changed = true;
    }
    if (m.walkieTalk && !walkieBroadcast) { m.walkieTalk = false; strcpy(m.lastNotice, "WiFi 未连接，无法对讲"); }
    walkieWant = m.walkieTalk;
    // The codec is free once the page it interrupted has let go of it.
    const bool codecTaken = soundRunning || noiseRunning || wakeRunning;
    if (!walkieRunning && codecFree && !codecTaken && m.walkiePhase != 3) {  // a failure waits for the page to open again
        const char* name = identity().valid && *identity().name ? identity().name : "胸牌"; walkieName(walkieSelfName, name, strlen(name));
        walkieStop = false; walkieRunning = true; walkiePhase = 0;
        // Opus needs a deep stack: chat measured 28 KB at its peak.
        if (xTaskCreate(taskMain<walkieRun>, "walkie", 32768, nullptr, 4, nullptr) != pdPASS) { walkieRunning = false; walkiePhase = 3; }
    }
    // The talk's clock on screen, once a second.
    static uint64_t shown = 0;
    if ((m.walkieTalk || m.walkiePhase == 2) && now != shown) { shown = now; changed = true; }
    return changed;
}
void walkieStatus(char* output, size_t size) {
    snprintf(output, size, "[%u,%u,%u,%u,%u,%u,%u,%d]", unsigned(walkieRxRunning.load()), walkieIn.load(), walkiePlayed.load(), walkieSent.load(),
             walkieErrors.load(), walkieStack.load(), walkieHeap.load(), walkieLastError.load());
}
bool wakeHolding() { return wakeRunning; }
void wakeStatus(char* output, size_t size) {
    snprintf(output, size, "[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", wakeState.load(), wakeError.load(), wakeHeap.load(), wakeLargest.load(),
             wakeChunk.load(), wakeFrames.load(), wakeDetects.load(), wakeUs.load(), wakeStack.load(), wakeAfterHeap.load(), wakeAfterLargest.load());
}
}
