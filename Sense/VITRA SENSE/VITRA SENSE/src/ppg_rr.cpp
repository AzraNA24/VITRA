#include <Arduino.h>
#include <math.h>
#include "ppg_rr.h"

void PPGRREstimator::begin(float rate) {
    sampleRate_ = rate >= 50.0f ? rate : 400.0f;
    // Same 2nd-order Butterworth coefficients as the original RR code.
    const float fc = 1.8f;
    const float k = tanf(PI * fc / sampleRate_);
    const float k2 = k * k;
    const float norm = 1.0f / (1.0f + 1.41421356f * k + k2);
    respB0_ = k2 * norm;
    respB1_ = 2.0f * respB0_;
    respB2_ = respB0_;
    respA1_ = 2.0f * (k2 - 1.0f) * norm;
    respA2_ = (1.0f - 1.41421356f * k + k2) * norm;
    reset();
}
void PPGRREstimator::reset() {
    respZ1_ = respZ2_ = respDenoised_ = 0.0f;
    respDenoiseInitialized_ = false;
    respGainBaseline_ = respGainCentered_ = 0.0f;
    respGainEnvelope_ = respGainReference_ = respGain_ = 1.0f;
    respGained_ = respGainedSmooth_ = 0.0f;
    respGainInitialized_ = false;
    respAdaptInitialized_ = respAdaptCycleEvent_ = false;
    respAdaptHaveTrough_ = respAdaptHavePeak_ = false;
    respAdaptDirection_ = respAdaptCandidateDirection_ = 0;
    respAdaptCandidateSamples_ = respAdaptSampleIndex_ = 0;
    respAdaptTroughSample_ = respAdaptLastEventSample_ = 0;
    respAdaptCycleCount_ = 0;
    respAdaptFastSmallStreak_ = 0;
    respAdaptPrevious_ = respAdaptCandidateExtreme_ = 0.0f;
    respAdaptTroughValue_ = respAdaptPeakValue_ = 0.0f;
    respAdaptAmplitudeReference_ = respAdaptLastAmplitude_ = 0.0f;
}
void PPGRREstimator::process(float respIR) {
    processRespDenoise(respIR);
    processRespGain(respDenoised_);
    processRespAdaptCycle(respGained_);
}
void PPGRREstimator::processRespDenoise(float respIR) {

    if (!isfinite(respIR))
        return;

    if (!respDenoiseInitialized_) {

        respDenoised_ = respIR;

        respZ1_ = (1.0f - respB0_) * respIR;
        respZ2_ = (respB2_ - respA2_) * respIR;

        respDenoiseInitialized_ = true;
        return;
    }

    const float output =
        respB0_ * respIR + respZ1_;

    respZ1_ =
        respB1_ * respIR
        - respA1_ * output
        + respZ2_;

    respZ2_ =
        respB2_ * respIR
        - respA2_ * output;

    respDenoised_ = output;
}

void PPGRREstimator::processRespGain(float input) {

    if (!isfinite(input))
        return;

    if (!respGainInitialized_) {
        respGainBaseline_ = input;
        respGainCentered_ = 0.0f;

        respGainEnvelope_ = 1.0f;
        respGainReference_ = 1.0f;

        respGain_ = 1.0f;
        respGained_ = input;

        respGainInitialized_ = true;
        return;
    }

    const float dt = 1.0f / sampleRate_;

    // 1. BASELINE TRACKING
    const float baselineBeta =
        1.0f - expf(-dt / 12.0f);

    respGainBaseline_ +=
        baselineBeta *
        (input - respGainBaseline_);

    respGainCentered_ =
        input - respGainBaseline_;

    // 2. LOCAL AMPLITUDE ENVELOPE
    const float envelopeBeta =
        1.0f - expf(-dt / 1.5f);

    respGainEnvelope_ +=
        envelopeBeta *
        (fabsf(respGainCentered_) -
         respGainEnvelope_);

    // 3. SLOW REFERENCE AMPLITUDE
    const float referenceBeta =
        1.0f - expf(-dt / 30.0f);

    respGainReference_ +=
        referenceBeta *
        (respGainEnvelope_ -
         respGainReference_);

    // 4. ADAPTIVE GAIN
    const float gainFloor =
        fmaxf(1.0f, respGainReference_ / 3.0f);

    const float gainTarget = clampf(
        respGainReference_ /
        fmaxf(respGainEnvelope_, gainFloor),
        1.0f,
        3.0f
    );

    // 5. SMOOTH GAIN TRANSITION
    const float gainBeta =
        1.0f - expf(-dt / 0.5f);

    respGain_ +=
        gainBeta * (gainTarget - respGain_);

    // 6. AMPLITUDE ENHANCEMENT
    respGained_ =
        respGainBaseline_ +
        respGainCentered_ * respGain_;

    // ==========================================
// POST-AGC LOW PASS FILTER
// First order, cutoff 1.5 Hz
// ==========================================

const float fc = 1.5f;

const float alpha =
    1.0f - expf(
        -2.0f * PI * fc / sampleRate_
    );

respGainedSmooth_ +=
    alpha * (respGained_ - respGainedSmooth_);
}

void PPGRREstimator::processRespAdaptCycle(float input) {
    respAdaptCycleEvent_ = false;
    if (!isfinite(input)) return;
    ++respAdaptSampleIndex_;
    if (!respAdaptInitialized_) {
        respAdaptPrevious_ = input;
        respAdaptCandidateExtreme_ = input;
        respAdaptInitialized_ = true;
        return;
    }
    const int direction = input > respAdaptPrevious_ ? 1 :
                          input < respAdaptPrevious_ ? -1 : 0;
    const float previous = respAdaptPrevious_;
    respAdaptPrevious_ = input;
    if (direction == 0) return;

    const uint32_t rounded = (uint32_t)(RESP_ADAPT_CONFIRM_SECONDS * sampleRate_ + 0.5f);
    const uint32_t needed = rounded > 0 ? rounded : 1;
    if (respAdaptDirection_ == 0) {
        if (direction == respAdaptCandidateDirection_) ++respAdaptCandidateSamples_;
        else { respAdaptCandidateDirection_ = direction; respAdaptCandidateSamples_ = 1; }
        if (respAdaptCandidateSamples_ >= needed) {
            respAdaptDirection_ = direction;
            respAdaptCandidateSamples_ = 0;
            respAdaptCandidateExtreme_ = input;
        }
        return;
    }
    if (direction == respAdaptDirection_) {
        respAdaptCandidateDirection_ = 0;
        respAdaptCandidateSamples_ = 0;
        // Track extreme for the currently confirmed trend.
        if (direction > 0 && input > respAdaptCandidateExtreme_) respAdaptCandidateExtreme_ = input;
        if (direction < 0 && input < respAdaptCandidateExtreme_) respAdaptCandidateExtreme_ = input;
        return;
    }
    if (direction != respAdaptCandidateDirection_) {
        respAdaptCandidateDirection_ = direction;
        respAdaptCandidateSamples_ = 1;
    } else ++respAdaptCandidateSamples_;
    if (respAdaptCandidateSamples_ < needed) return;

    const int oldDirection = respAdaptDirection_;
    const float extreme = respAdaptCandidateExtreme_;
    respAdaptDirection_ = direction;
    respAdaptCandidateDirection_ = 0;
    respAdaptCandidateSamples_ = 0;
    respAdaptCandidateExtreme_ = input;

    if (oldDirection == -1 && direction == 1) {
        // The preceding falling trend ended at a local trough.
        const uint32_t troughSample = respAdaptSampleIndex_ - needed;
        if (respAdaptHaveTrough_ && respAdaptHavePeak_) {
            const float amplitude = respAdaptPeakValue_ - respAdaptTroughValue_;
            if (amplitude > 0.0f) {
                respAdaptLastAmplitude_ = amplitude;
                const float elapsed = (troughSample - respAdaptLastEventSample_) / sampleRate_;
                const bool periodOK = !respAdaptCycleCount_ ||
                    (elapsed >= RESP_RAW_MIN_PERIOD_SECONDS && elapsed <= RESP_RAW_MAX_PERIOD_SECONDS);
                const bool strong = respAdaptAmplitudeReference_ <= 0.0f ||
                    amplitude >= RESP_ADAPT_PROMINENCE_RATIO * respAdaptAmplitudeReference_;
                // A sudden transition from slow/large to fast/small breaths
                // needs two consecutive smaller, fast candidates before rebasing.
                if (!strong && periodOK && elapsed <= 3.0f &&
                    amplitude >= 0.15f * respAdaptAmplitudeReference_) {
                    if (respAdaptFastSmallStreak_ < 2) ++respAdaptFastSmallStreak_;
                } else if (strong || !periodOK) {
                    respAdaptFastSmallStreak_ = 0;
                }
                const bool fastTransition = respAdaptFastSmallStreak_ >= 2;
                if (periodOK && (strong || fastTransition)) {
                    respAdaptCycleEvent_ = true;
                    ++respAdaptCycleCount_;
                    respAdaptLastEventSample_ = troughSample;
                    if (respAdaptAmplitudeReference_ <= 0.0f || fastTransition)
                        respAdaptAmplitudeReference_ = amplitude;
                    else
                        respAdaptAmplitudeReference_ = 0.8f * respAdaptAmplitudeReference_ + 0.2f * amplitude;
                    respAdaptFastSmallStreak_ = 0;
                }
            }
        }
        respAdaptTroughValue_ = extreme;
        respAdaptTroughSample_ = troughSample;
        respAdaptHaveTrough_ = true;
        respAdaptHavePeak_ = false;
    } else if (oldDirection == 1 && direction == -1 && respAdaptHaveTrough_) {
        respAdaptPeakValue_ = extreme;
        respAdaptHavePeak_ = true;
    }
}