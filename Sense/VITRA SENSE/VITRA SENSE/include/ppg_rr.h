#ifndef PPG_RR_H
#define PPG_RR_H
#include <Arduino.h>

// Only the selected adaptive-amplitude respiratory cycle detector remains.
class PPGRREstimator {
public:
    void begin(float rawSampleRateHz = 400.0f);
    void reset();
    void process(float respIR);
    uint32_t getRespAdaptCycleCount() const { return respAdaptCycleCount_; }
private:
    static constexpr float RESP_ADAPT_CONFIRM_SECONDS = 0.200f;
    static constexpr float RESP_ADAPT_PROMINENCE_RATIO = 0.35f;
    static constexpr float RESP_RAW_MIN_PERIOD_SECONDS = 60.0f / 70.0f;
    static constexpr float RESP_RAW_MAX_PERIOD_SECONDS = 10.0f;
    float sampleRate_ = 400.0f;
    float respB0_ = 0, respB1_ = 0, respB2_ = 0, respA1_ = 0, respA2_ = 0;
    float respZ1_ = 0, respZ2_ = 0, respDenoised_ = 0;
    bool respDenoiseInitialized_ = false;
    float respGainBaseline_ = 0, respGainCentered_ = 0;
    float respGainEnvelope_ = 1, respGainReference_ = 1;
    float respGain_ = 1, respGained_ = 0, respGainedSmooth_ = 0;
    bool respGainInitialized_ = false;
    bool respAdaptInitialized_ = false, respAdaptCycleEvent_ = false;
    bool respAdaptHaveTrough_ = false, respAdaptHavePeak_ = false;
    int respAdaptDirection_ = 0, respAdaptCandidateDirection_ = 0;
    uint32_t respAdaptCandidateSamples_ = 0, respAdaptSampleIndex_ = 0;
    uint32_t respAdaptTroughSample_ = 0, respAdaptLastEventSample_ = 0;
    uint32_t respAdaptCycleCount_ = 0;
    uint8_t respAdaptFastSmallStreak_ = 0;
    float respAdaptPrevious_ = 0, respAdaptCandidateExtreme_ = 0;
    float respAdaptTroughValue_ = 0, respAdaptPeakValue_ = 0;
    float respAdaptAmplitudeReference_ = 0, respAdaptLastAmplitude_ = 0;
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    void processRespDenoise(float respIR);
    void processRespGain(float input);
    void processRespAdaptCycle(float input);
};
#endif
