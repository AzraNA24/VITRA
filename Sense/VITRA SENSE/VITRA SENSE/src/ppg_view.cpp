#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "ppg_filter.h"
#include "ppg_processor.h"
#include "ppg_rr.h"
#include "ppg_view.h"

static MAX30105 particleSensor;
static PPGFilter ppgFilter;
static PPGProcessor ppgProcessor;
static PPGRREstimator rrEstimator;
static float currentBPM = 0.0f;
static float currentSpO2 = 0.0f;
static float currentRR = 0.0f;
static bool rrReady = false;
static bool sensorConnected = false;
static bool fingerDetected = false;
static constexpr uint32_t FINGER_IR_THRESHOLD = 10000;
static constexpr uint32_t OUTPUT_INTERVAL_MS = 1000;
static constexpr uint32_t RR_WINDOW_MS = 60000;
static uint32_t lastOutputTime = 0;
static uint32_t rrWindowStart = 0;
static uint32_t rrWindowStartCount = 0;

bool ppgBegin() {
    sensorConnected = particleSensor.begin(Wire, I2C_SPEED_FAST);
    if (!sensorConnected) {
        Serial.println("MAX30102 NOT FOUND");
        return false;
    }
    // Preserve original MAX30102 settings.
    byte powerLevel = 0x1F;
    byte sampleAverage = 1;
    byte ledMode = 2;
    int sampleRate = 400;
    int pulseWidth = 411;
    int adcRange = 4096;
    particleSensor.setup(powerLevel, sampleAverage, ledMode,
                         sampleRate, pulseWidth, adcRange);
    rrEstimator.begin(400.0f);
    lastOutputTime = millis();
    Serial.println("VITRA PPG READY");
    return true;
}

bool ppgUpdate() {
    if (!sensorConnected) return false;
    bool adaHasilBaru = false;
    particleSensor.check();
    while (particleSensor.available()) {
        const uint32_t rawRed = particleSensor.getFIFORed();
        const uint32_t rawIR = particleSensor.getFIFOIR();
        if (rawIR < FINGER_IR_THRESHOLD) {
            if (fingerDetected) {
                fingerDetected = false;
                currentBPM = currentSpO2 = currentRR = 0.0f;
                rrReady = false;
                ppgFilter.reset();
                ppgProcessor.reset();
                rrEstimator.reset();
                rrWindowStart = 0;
                rrWindowStartCount = 0;
                Serial.println("PPG: FINGER REMOVED");
            }
            particleSensor.nextSample();
            continue;
        }
        if (!fingerDetected) {
            fingerDetected = true;
            rrReady = false;
            rrWindowStart = millis();
            rrWindowStartCount = rrEstimator.getRespAdaptCycleCount();
        }
        float cleanRed = 0.0f, cleanIR = 0.0f, respIR = 0.0f;
        ppgFilter.process((float)rawRed, (float)rawIR,
                          cleanRed, cleanIR, respIR);
        float legacyRR = 0.0f; // Existing HR/SpO2 processor signature, unused.
        const bool newBeat = ppgProcessor.processSample(
            cleanRed, cleanIR, respIR,
            ppgFilter.getDCRed(), ppgFilter.getDCIR(),
            currentBPM, currentSpO2, legacyRR);
        if (newBeat) adaHasilBaru = true;
        rrEstimator.process(respIR);
        particleSensor.nextSample();
    }
    const uint32_t now = millis();
    if (fingerDetected && (uint32_t)(now - rrWindowStart) >= RR_WINDOW_MS) {
        const uint32_t count = rrEstimator.getRespAdaptCycleCount();
        currentRR = (float)(count - rrWindowStartCount);
        rrReady = true;
        rrWindowStartCount = count;
        rrWindowStart += RR_WINDOW_MS;
    }
    if ((uint32_t)(now - lastOutputTime) >= OUTPUT_INTERVAL_MS) {
        lastOutputTime = now;
        if (fingerDetected) {
            Serial.print("HR: "); Serial.print(currentBPM, 1);
            Serial.print(" | SpO2: "); Serial.print(currentSpO2, 1);
            Serial.print(" | RR: ");
            if (rrReady) { Serial.print(currentRR, 0); Serial.println(" RPM"); }
            else Serial.println("Analyzing...");
        }
    }
    return adaHasilBaru;
}
bool ppgConnected() { return sensorConnected; }
bool ppgFingerDetected() { return fingerDetected; }
float ppgGetBPM() { return currentBPM; }
float ppgGetSpO2() { return currentSpO2; }
float ppgGetRR() { return currentRR; }
