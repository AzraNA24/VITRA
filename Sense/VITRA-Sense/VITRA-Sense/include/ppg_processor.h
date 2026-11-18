#ifndef PPG_PROCESSOR_H
#define PPG_PROCESSOR_H

#include <Arduino.h>

class PPGProcessor {
private:
  static const int N_W1 = 44;
  static const int N_W2 = 267;
  float ringW1[N_W1] = {0.0f};
  float ringW2[N_W2] = {0.0f};
  int idxW1 = 0, idxW2 = 0;
  float sumW1 = 0.0f, sumW2 = 0.0f;
  const float beta = 0.02f;
  float meanSignal = 0.0f;
  float alpha = 0.0f;
  bool inBlock = false;
  int currentBlockWidth = 0;
  float blockMaxVal = -9999.0f;
  uint32_t blockMaxTime = 0;
  static const int BPM_BUFFER_SIZE = 8;
  float bpmBuffer[BPM_BUFFER_SIZE] = {0.0f};
  int bpmIndex = 0;
  uint32_t lastRespTime = 0;
  float peakThresholdRR = 0.5f;
  bool isRespPeak = false;
  float currentRR = 16.0f;
  float maxRedAC = -9999.0f, minRedAC = 9999.0f;
  float maxIRAC = -9999.0f, minIRAC = 9999.0f;
  uint32_t lastBeatTime = 0;
  const float maxBPMRate = 10.0f;
  const float maxRRRate = 2.0f;
  const float maxSpO2Rate = 2.0f;
  static const int MAX_REJECT = 5;
  float lastBPM = 0.0f, lastSpO2 = 0.0f;
  uint32_t lastBPMTime = 0, lastSpO2Time = 0, lastRRTime = 0;
  int bpmRejectCount = 0, spo2RejectCount = 0, rrRejectCount = 0;
  bool rrInit = false;

  bool withinLimit(float newVal, float lastVal, float ratePerSec, uint32_t dtMs) {
    float dt = fmaxf((float)dtMs / 1000.0f, 1.0f);
    return fabsf(newVal - lastVal) <= ratePerSec * dt;
  }

public:
  PPGProcessor();
  bool processSample(float filteredRed, float filteredIR, float respIR,
                     float dcRed, float dcIR, float &outBPM,
                     float &outSpO2, float &outRR);
  void reset();
};

#endif
