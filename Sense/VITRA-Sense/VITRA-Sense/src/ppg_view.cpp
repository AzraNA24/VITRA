#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "ppg_filter.h"
#include "ppg_processor.h"
#include "ppg_view.h"

static MAX30105 particleSensor;
static PPGFilter ppgFilter;
static PPGProcessor ppgProcessor;

static float currentBPM = 0.0f;
static float currentSpO2 = 0.0f;
static float currentRR = 0.0f;
static bool sensorConnected = false;
static bool fingerDetected = false;
static const uint32_t FINGER_IR_THRESHOLD = 10000;

bool ppgBegin() {
  // Wire sudah dimulai oleh setup() utama pada apaaaa.ino.
  // Jangan panggil Wire.begin() lagi di file ini.
  sensorConnected = particleSensor.begin(Wire, I2C_SPEED_FAST);
  if (!sensorConnected) {
    Serial.println("Sensor MAX30102 tidak ditemukan!");
    return false;
  }

  byte powerLevel = 0x1F;
  byte sampleAverage = 1;
  byte ledMode = 2;
  int sampleRate = 400;
  int pulseWidth = 411;
  int adcRange = 4096;

  particleSensor.setup(
    powerLevel,
    sampleAverage,
    ledMode,
    sampleRate,
    pulseWidth,
    adcRange
  );
  Serial.println("Sistem PPG siap...");
  return true;
}

bool ppgUpdate() {
  if (!sensorConnected) return false;

  bool adaHasilBaru = false;
  particleSensor.check();

  while (particleSensor.available()) {
    uint32_t rawRed = particleSensor.getFIFORed();
    uint32_t rawIR = particleSensor.getFIFOIR();

    if (rawIR < FINGER_IR_THRESHOLD) {
      if (fingerDetected) {
        fingerDetected = false;
        currentBPM = 0.0f;
        currentSpO2 = 0.0f;
        currentRR = 0.0f;
        ppgFilter.reset();
        ppgProcessor.reset();
        Serial.println("PPG: JARI TIDAK TERDETEKSI");
      }
      particleSensor.nextSample();
      continue;
    }

    fingerDetected = true;
    float cleanRed = 0.0f;
    float cleanIR = 0.0f;
    float respIR = 0.0f;

    ppgFilter.process(
      (float)rawRed,
      (float)rawIR,
      cleanRed,
      cleanIR,
      respIR
    );

    bool newBeat = ppgProcessor.processSample(
      cleanRed,
      cleanIR,
      respIR,
      ppgFilter.getDCRed(),
      ppgFilter.getDCIR(),
      currentBPM,
      currentSpO2,
      currentRR
    );

    Serial.print(">Raw_IR:");
    Serial.println(rawIR);
    Serial.print(">Clean_IR:");
    Serial.println(cleanIR);

    if (newBeat) {
      adaHasilBaru = true;
      Serial.print("Heart Rate: ");
      Serial.print(currentBPM, 0);
      Serial.print(" BPM | SpO2: ");
      Serial.print(currentSpO2, 0);
      Serial.print(" % | Resp Rate: ");
      Serial.print(currentRR, 0);
      Serial.println(" RPM");
    }

    particleSensor.nextSample();
  }

  return adaHasilBaru;
}

bool ppgConnected() {
  return sensorConnected;
}

bool ppgFingerDetected() {
  return fingerDetected;
}

float ppgGetBPM() {
  return currentBPM;
}

float ppgGetSpO2() {
  return currentSpO2;
}

float ppgGetRR() {
  return currentRR;
}