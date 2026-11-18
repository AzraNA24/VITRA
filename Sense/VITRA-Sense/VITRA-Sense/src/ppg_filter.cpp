#include "ppg_filter.h"

ButterworthBandpass::ButterworthBandpass() { reset(); }

void ButterworthBandpass::reset() {
  x1 = 0.0f;
  x2 = 0.0f;
  y1 = 0.0f;
  y2 = 0.0f;
}

float ButterworthBandpass::process(float inSample) {
  float outSample = b0 * inSample + b1 * x1 + b2 * x2
                  - a1 * y1 - a2 * y2;
  x2 = x1;
  x1 = inSample;
  y2 = y1;
  y1 = outSample;
  return outSample;
}

ButterworthLowpassResp::ButterworthLowpassResp() { reset(); }

void ButterworthLowpassResp::reset() {
  x1 = 0.0f;
  x2 = 0.0f;
  y1 = 0.0f;
  y2 = 0.0f;
}

float ButterworthLowpassResp::process(float inSample) {
  float outSample = b0 * inSample + b1 * x1 + b2 * x2
                  - a1 * y1 - a2 * y2;
  x2 = x1;
  x1 = inSample;
  y2 = y1;
  y1 = outSample;
  return outSample;
}

PPGFilter::PPGFilter() { reset(); }

void PPGFilter::reset() {
  dcRed = 0.0f;
  dcIR = 0.0f;
  filterRed.reset();
  filterIR.reset();
  filterRespIR.reset();
}

void PPGFilter::process(float rawRed, float rawIR, float &filteredRed,
                        float &filteredIR, float &respIR) {
  if (dcRed == 0.0f) dcRed = rawRed;
  if (dcIR == 0.0f) dcIR = rawIR;
  dcRed = alphaDC * dcRed + (1.0f - alphaDC) * rawRed;
  dcIR = alphaDC * dcIR + (1.0f - alphaDC) * rawIR;
  float acRed = rawRed - dcRed;
  float acIR = rawIR - dcIR;
  filteredRed = filterRed.process(acRed);
  filteredIR = filterIR.process(acIR);
  respIR = filterRespIR.process(acIR);
}
