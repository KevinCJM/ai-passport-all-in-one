#pragma once
#include <cstdint>
#include <cstring>
namespace passport {
// Aligned 8kHz microphone/reference samples, bounded 40ms echo path.
// This gate is not a wake-word engine. Physical gain/delay need calibration.
template<int FRAME,int TAPS,int UPDATE=1,int FLOOR=40,bool CALIBRATE=false,int REQUIRED=3> class VoiceGateImpl {
    static constexpr int WARMUP=50;
    int32_t weights[TAPS]{};
    int16_t history[TAPS]{};
    uint32_t energy{}, frames{}, freeze{}, consecutive{};
    uint16_t position{};
    int16_t previous_error{};
    uint32_t echoNoise_{};
public:
    uint32_t residual_power{};
    uint32_t mic_power{},reference_power{},warm_frames{},blocked{},echo_floor{};
    void reset() { *this=VoiceGateImpl{}; }
    int process(const int16_t *ref,const int16_t *mic,int16_t *clean) {
    int warming = frames < WARMUP;
    int64_t incoming_mic = 0, incoming_reference = energy;
    for (int n = 0; n < FRAME; ++n) {
        int32_t m = mic[n] / 16, x = ref[n] / 16;
        incoming_mic += m * m; incoming_reference += x * x;
    }
    /* Cold-start protection: a strongly dominant microphone signal must not
     * train an uninitialised echo filter on the user's first syllable. Include
     * all 40 ms of reference history so a delayed echo tail is not mistaken
     * for near speech simply because the current output frame became quiet. */
    /* This is an energy bound over the whole delayed-reference support, not
     * a comparison of RMS values from differently sized windows. For the lab
     * echo paths with sum(abs(gain)) <= 1, echo energy cannot exceed this
     * reference bound. A violation protects a cold near utterance until the
     * fixed convergence interval ends. Physical gains require calibration. */
    int dominant_near = incoming_mic > incoming_reference + FRAME * FLOOR * FLOOR;
    if (warming && dominant_near) freeze = WARMUP - frames;
    int adapt = !freeze && !dominant_near;
    int64_t residual_energy = 0, mic_energy = 0, prediction_energy = 0;
    uint32_t crossings = 0;
    for (int n = 0; n < FRAME; ++n) {
        int cursor = position;
        int32_t old = history[cursor], x = ref[n] / 16, m = mic[n] / 16;
        energy -= old * old;
        history[cursor] = (int16_t)x;
        energy += x * x;
        int j = cursor;
        int64_t estimate = 0;
        for (int k = 0; k < TAPS; ++k) {
            // Q15 weight * PCM/16 fits int32; only the sum needs 64 bits.
            estimate += weights[k] * history[j];
            if (--j < 0) j = TAPS - 1;
        }
        int32_t prediction = (int32_t)(estimate >> 15);
        if (prediction > 4096) prediction = 4096;
        if (prediction < -4096) prediction = -4096;
        int32_t error = m - prediction;
        int32_t restored = error * 16;
        if (restored > 32767) restored = 32767;
        if (restored < -32768) restored = -32768;
        clean[n] = (int16_t)restored;
        residual_energy += (int64_t)error * error;
        mic_energy += (int64_t)m * m;
        prediction_energy += (int64_t)prediction * prediction;
        if ((error < 0 && previous_error > 0) ||
            (error > 0 && previous_error < 0)) crossings++;
        if (error) previous_error = (int16_t)error;
        if (adapt && energy > 4096 && n % UPDATE == 0) {
            /* mu=0.75. Reciprocal uses hardware integer division once per
             * update, not a division per tap. All products fit signed int64. */
            uint32_t reciprocal = (1u << 30) / (energy + 4096);
            if (!reciprocal) reciprocal = 1;
            const int64_t step = (int64_t)error * reciprocal * 24576;
            j = cursor;
            for (int k = 0; k < TAPS; ++k) {
                int64_t delta = step * history[j];
                /* Symmetric rounding avoids a -1 bias on every negative
                 * sub-LSB update across this long, highly correlated filter. */
                int64_t rounded = delta >= 0 ? (delta + (1LL << 29)) >> 30
                                             : -((-delta + (1LL << 29)) >> 30);
                int64_t weight = weights[k] + rounded;
                if (weight > 32768) weight = 32768;
                if (weight < -32768) weight = -32768;
                weights[k] = (int32_t)weight;
                if (--j < 0) j = TAPS - 1;
            }
        }
        if (++cursor == TAPS) cursor = 0;
        position = (uint16_t)cursor;
    }
    // Cloud TTS-start can precede its first audio packet by over a second.
    // Silence cannot warm an echo filter that has not received a reference.
    if(incoming_reference>4096)frames++;
    residual_power = (uint32_t)(residual_energy / FRAME);
    // Learn only the settled, echo-only part of warmup, not a cold near voice.
    if(CALIBRATE&&warming&&frames>=WARMUP/2&&!dominant_near&&residual_power>echoNoise_)echoNoise_=residual_power;
    echo_floor=FLOOR*FLOOR;
    if(CALIBRATE&&echoNoise_*9/4>echo_floor)echo_floor=echoNoise_*9/4;
    // FLOOR uses PCM/16 units. At full speaker volume a near voice must also
    // dominate the recent output reference; residual echo alone is insufficient.
    const uint32_t reference_floor = incoming_reference * 16 / ((TAPS + FRAME) * 25);
    mic_power=mic_energy/FRAME;reference_power=incoming_reference/(TAPS+FRAME);warm_frames=frames;
    blocked=(residual_power<echo_floor?1U:0U)|(mic_power<FLOOR*FLOOR?2U:0U)
          |(residual_power<reference_floor?4U:0U)|(crossings<2||crossings>60?8U:0U)
          |(residual_energy*25<mic_energy*16?16U:0U);
    int activity = !blocked;
    if (!warming && activity && mic_energy > 2 * prediction_energy + FRAME * 100)
        freeze = 6;
    else if (freeze) freeze--;
    if (warming && energy > 4096 && !dominant_near) {activity = 0;blocked|=32;}
    if (activity) {
        if (consecutive < REQUIRED) consecutive++;
    } else consecutive = 0;
    if (consecutive < REQUIRED) return 0;
    return 1;
    }
};
using VoiceGate=VoiceGateImpl<160,321>;
using VoiceGate4k=VoiceGateImpl<80,161>;
// The physical codec uses 6dB during playback, 18dB below capture gain.
// Measured speaker-only residual peaked at RMS363 with the old RMS160 gate.
// Use a RMS384 minimum, learn residual echo during warmup, confirm for 100ms.
// An 80ms acoustic path; alternate-sample updates leave CPU for Opus.
using VoiceGate2k=VoiceGateImpl<40,161,2,24,true,5>;
}
