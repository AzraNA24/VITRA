#include "ppg_processor.h"

PPGProcessor::PPGProcessor() { reset(); }

void PPGProcessor::reset() {
  lastBeatTime = 0;
  lastRespTime = 0;
  isRespPeak = false;
  currentRR = 16.0f;
  rrIdxShort = 0;
  rrIdxLong = 0;

  rrSumShort = 0.0f;
  rrSumLong = 0.0f;

  rrMeanEnergy = 0.0f;
  rrAlpha = 0.0f;
  rrThreshold = 0.0f;

  rrPolarity = 0;
  lastRREnergy = 0.0f;
  lastRRShort = 0.0f;
  lastRRLong = 0.0f;
  lastRRActivity = false;
  lastRREvent = false;
  lastRRPolarity = 0;

  for (int i = 0; i < N_RR_SHORT; i++) {
    rrRingShort[i] = 0.0f;
  }

  for (int i = 0; i < N_RR_LONG; i++) {
    rrRingLong[i] = 0.0f;
  }

  bpmIndex = 0;
  inBlock = false;
  currentBlockWidth = 0;
  blockMaxVal = -9999.0f;
  blockMaxTime = 0;
  meanSignal = 0.0f;
  alpha = 0.0f;
  sumW1 = 0.0f;
  sumW2 = 0.0f;
  idxW1 = 0;
  idxW2 = 0;
  for (int i = 0; i < N_W1; i++) ringW1[i] = 0.0f;
  for (int i = 0; i < N_W2; i++) ringW2[i] = 0.0f;
  for (int i = 0; i < BPM_BUFFER_SIZE; i++) bpmBuffer[i] = 0.0f;
  maxRedAC = -9999.0f;
  minRedAC = 9999.0f;
  maxIRAC = -9999.0f;
  minIRAC = 9999.0f;
  lastBPM = 0.0f;
  lastSpO2 = 0.0f;
  lastBPMTime = 0;
  lastSpO2Time = 0;
  lastRRTime = 0;
  bpmRejectCount = 0;
  spo2RejectCount = 0;
  rrRejectCount = 0;
  rrInit = false;
}

bool PPGProcessor::processSample(float filteredRed, float filteredIR,
                                 float respIR, float dcRed, float dcIR,
                                 float &outBPM, float &outSpO2,
                                 float &outRR) {
  uint32_t currentTime = millis();
  bool updatedHR = false;

  //bagian RR
  // BAGIAN RR - MONITORING
  lastRREvent = false;

  float rrEnergy = respIR * respIR;
  rrSumShort -= rrRingShort[rrIdxShort];
  rrRingShort[rrIdxShort] = rrEnergy;
  rrSumShort += rrEnergy;
  rrIdxShort = (rrIdxShort + 1) % N_RR_SHORT;
  float maRRShort = rrSumShort / (float)N_RR_SHORT;

  rrSumLong -= rrRingLong[rrIdxLong];
  rrRingLong[rrIdxLong] = rrEnergy;
  rrSumLong += rrEnergy;
  rrIdxLong = (rrIdxLong + 1) % N_RR_LONG;
  float maRRLong = rrSumLong / (float)N_RR_LONG;
  // Simpan nilai untuk monitoring
  lastRREnergy = rrEnergy;
  lastRRShort = maRRShort;
  lastRRLong = maRRLong;

  rrMeanEnergy = 0.99f * rrMeanEnergy
             + 0.01f * rrEnergy;

  rrAlpha = betaRR * rrMeanEnergy;
  rrThreshold = maRRLong + rrAlpha;

  bool rrActivity = (maRRShort > rrThreshold);
  lastRRActivity = rrActivity;
  int currentPolarity = 0;
  if (respIR > 0.0f) {
    currentPolarity = 1;
  }
  else if (respIR < 0.0f) {
    currentPolarity = -1;
  }

  lastRRPolarity = currentPolarity;

  if (rrActivity && rrPolarity == 0 && !isRespPeak) {
    rrPolarity = currentPolarity;
  }

  if (rrActivity &&
    !isRespPeak &&
    rrPolarity != 0 &&
    currentPolarity == rrPolarity) {
    lastRREvent = true;

    isRespPeak = true;

    if (lastRespTime > 0) {

        uint32_t respInterval = currentTime - lastRespTime;

        // Interval minimum untuk target RR maksimum 70
        if (respInterval >= 857) {

            float rawRR = 60000.0f / (float)respInterval;

            if (rawRR >= 4.0f && rawRR <= 70.0f) {

                bool accept = !rrInit ||
                    withinLimit(
                        rawRR,
                        currentRR,
                        maxRRRate,
                        currentTime - lastRRTime
                    );

                if (!accept && ++rrRejectCount >= MAX_REJECT) {
                    accept = true;
                }

                if (accept) {
                    currentRR = rawRR;
                    rrInit = true;
                    rrRejectCount = 0;
                    lastRRTime = currentTime;
                }
            }
        }
    }

    lastRespTime = currentTime;
}

  if (currentPolarity != rrPolarity) {
    isRespPeak = false;
}

  outRR = currentRR;

  //bagian HR
  float clippedIR = filteredIR > 0.0f ? filteredIR : 0.0f;
  float squaredIR = clippedIR * clippedIR;

  sumW1 -= ringW1[idxW1];
  ringW1[idxW1] = squaredIR;
  sumW1 += squaredIR;
  idxW1 = (idxW1 + 1) % N_W1;
  float maPeak = sumW1 / (float)N_W1;

  sumW2 -= ringW2[idxW2];
  ringW2[idxW2] = squaredIR;
  sumW2 += squaredIR;
  idxW2 = (idxW2 + 1) % N_W2;
  float maBeat = sumW2 / (float)N_W2;

  meanSignal = 0.99f * meanSignal + 0.01f * squaredIR;
  alpha = beta * meanSignal;
  float thr1 = maBeat + alpha;



  //bagian SpO2
  if (filteredRed > maxRedAC) maxRedAC = filteredRed;
  if (filteredRed < minRedAC) minRedAC = filteredRed;
  if (filteredIR > maxIRAC) maxIRAC = filteredIR;
  if (filteredIR < minIRAC) minIRAC = filteredIR;

  if (maPeak > thr1) {
    if (!inBlock) {
      inBlock = true;
      currentBlockWidth = 0;
      blockMaxVal = -9999.0f;
    }
    currentBlockWidth++;
    if (filteredIR > blockMaxVal) {
      blockMaxVal = filteredIR;
      blockMaxTime = currentTime;
    }
  } else if (inBlock) {
    if (currentBlockWidth >= N_W1) {
      uint32_t interval = blockMaxTime - lastBeatTime;
      if (interval >= 240 && lastBeatTime > 0) { //this
        float rawBPM = 60000.0f / (float)interval;
        bool accept = true;


        //limitasi
        if (lastBPM > 0.0f) {
          accept = withinLimit(rawBPM, lastBPM, maxBPMRate,
                               blockMaxTime - lastBPMTime);
        }
        if (!accept && ++bpmRejectCount >= MAX_REJECT) {
          accept = true;
          for (int i = 0; i < BPM_BUFFER_SIZE; i++) bpmBuffer[i] = 0.0f;
          bpmIndex = 0;
        }
        if (accept) {
          bpmRejectCount = 0;
          bpmBuffer[bpmIndex] = rawBPM;
          bpmIndex = (bpmIndex + 1) % BPM_BUFFER_SIZE;
          float sumBPM = 0.0f;
          int count = 0;
          for (int i = 0; i < BPM_BUFFER_SIZE; i++) {
            if (bpmBuffer[i] > 0.0f) {
              sumBPM += bpmBuffer[i];
              count++;
            }
          }
          outBPM = count > 0 ? sumBPM / count : rawBPM;
          lastBPM = outBPM;
          lastBPMTime = blockMaxTime;

          float currentACRed = maxRedAC - minRedAC;
          float currentACIR = maxIRAC - minIRAC;
          if (dcRed > 0.0f && dcIR > 0.0f && currentACIR > 0.0f) {
            float rRatio = (currentACRed / dcRed) / (currentACIR / dcIR);
            float calc = 104.0f - 17.0f * rRatio;
            if (calc > 100.0f) calc = 100.0f;
            if (calc < 70.0f) calc = 70.0f;
            if (lastSpO2 == 0.0f ||
                withinLimit(calc, lastSpO2, maxSpO2Rate,
                            blockMaxTime - lastSpO2Time) ||
                ++spo2RejectCount >= MAX_REJECT) {
              outSpO2 = calc;
              lastSpO2 = calc;
              lastSpO2Time = blockMaxTime;
              spo2RejectCount = 0;
            }
          }
          lastBeatTime = blockMaxTime;
          updatedHR = true;
        } else {
          lastBeatTime = blockMaxTime;
        }
        maxRedAC = -9999.0f;
        minRedAC = 9999.0f;
        maxIRAC = -9999.0f;
        minIRAC = 9999.0f;
      } else if (lastBeatTime == 0) {
        lastBeatTime = blockMaxTime;
      }
    }
    inBlock = false;
    currentBlockWidth = 0;
  }
  return updatedHR;
}
