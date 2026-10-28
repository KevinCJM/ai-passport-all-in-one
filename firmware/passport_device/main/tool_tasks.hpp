#pragma once
#include "suite.hpp"
#include "esp_netif.h"
namespace passport::tools {
// WiFi 工具箱 and 声音 do their slow work (scans, pings, the microphone, the metronome) on a task of their own
// while the page is open. Called from the main loop before LiveServices; true when the page should redraw.
bool wifiTick(Runtime& runtime,esp_netif_t* station,bool connected);
// 移动监控: CSI from the router while the page is open; a new movement wakes the screen.
bool motionTick(Runtime& runtime,esp_netif_t* station,bool connected);
// awake: the page will still be awake a moment from now (board::power(2) closes the codec on its own).
bool soundTick(Runtime& runtime,bool awake);
// 白噪音 and a ringing 闹钟 play on a task of their own, made up as they play (noise_gen.hpp); it keeps playing
// with the screen off. codecFree: no native task holds the speaker.
bool noiseTick(Runtime& runtime,bool codecFree);
// 语音唤醒: listens for 你好小智 while listen is true (the setting is on and the home page is idle); hearing it
// opens 小智语音, which starts listening at once. True when the page changed.
bool wakeTick(Runtime& runtime,bool listen);
// True from the wake task's start until its memory is freed: NativeServices waits for that before starting.
bool wakeHolding();
// Diagnostics for /status: "wake":[state,error,heap after create,largest,chunk,frames,detects,detect us,stack,
// heap and largest block just after the last wake word was heard and the task had freed its memory].
void wakeStatus(char* output,size_t size);
}
