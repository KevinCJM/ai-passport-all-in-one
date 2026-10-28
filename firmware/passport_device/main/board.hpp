#pragma once
#include "esp_http_server.h"
#include "settings.hpp"
#include "cw2017.hpp"
#include <cstddef>
#include <cstdint>
namespace passport::board {
void init(SystemSettings &settings);
void draw(int x1, int y1, int x2, int y2, uint8_t *pixels);
// Paints the whole screen one RGB565 colour by sending one band of it down every row.
void fill(uint16_t color, uint16_t *band, int rows);
// Switches the backlight without touching the saved brightness: the whole screen at once.
void backlight(bool on);
void keyMode(bool volumeKeys,uint32_t generation,bool awake,uint32_t pairingEpoch,bool homeScreen);
const char *key();
uint32_t keyContext();
bool usbConnected();
void pollBattery();
BatteryReading battery();
void batteryDiagnostics(char*,size_t);
void power(unsigned state);
bool setBrightness(unsigned value);
const char* bluetoothName();
void diagnostics(char *output, size_t size);
bool authorized(httpd_req_t *request);
void event(const char *operation, unsigned request, unsigned app, unsigned generation);
esp_err_t events(httpd_req_t *request);
void nextEvent(char*,size_t);
esp_err_t pair(httpd_req_t *request);
bool pairCode(char* code,uint32_t* epoch=nullptr);
void pairKey(bool allow,uint32_t expectedEpoch);
bool audioOpen(bool captureEnabled=true,unsigned captureFrames=2,bool alignedReference=false);
unsigned volume();
unsigned microphoneGain();
bool setVolume(unsigned value);
// The open codec's output level for now only (not saved): an alarm must not ring at a whisper.
void outputVolume(unsigned value);
// Deep sleep also ends after this many seconds (0 = only a key wakes it): the alarm.
void wakeAfter(uint32_t seconds);
void audioClose();
bool microphone(int16_t *samples, int16_t *reference);
bool playbackReference(bool enabled);
void referenceStats(unsigned& rate,unsigned& dropped);
bool speaker(const int16_t *samples, size_t count, unsigned rate, unsigned channels);
bool bufferedSpeakerStart();
void bufferedSpeakerFinish();
bool bufferedSpeakerDrained();
void bufferedSpeakerSnapshot(char*,size_t);
bool bufferedSpeakerStop();
void discardMicrophone();
void captureRecording(bool enabled);
void silence();
int audioListen();
const char *apiToken();
const char *apName();
bool provisioning();
void provisioning(bool value);
} // namespace passport::board
