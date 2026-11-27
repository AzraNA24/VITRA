#include <Arduino.h>
#include <math.h>
#include "ppg_filter.h"

// ==========================================
// BUTTERWORTH BANDPASS
// ==========================================

ButterworthBandpass::ButterworthBandpass() {
  reset();
}

void ButterworthBandpass::reset() {
  x1 = 0.0f;
  x2 = 0.0f;
  y1 = 0.0f;
  y2 = 0.0f;
}

float ButterworthBandpass::process(float inSample) {
  float outSample =
      b0 * inSample
    + b1 * x1
    + b2 * x2
    - a1 * y1
    - a2 * y2;

  x2 = x1;
  x1 = inSample;
  y2 = y1;
  y1 = outSample;

  return outSample;
}

// ==========================================
// BUTTERWORTH LOWPASS
// ==========================================

ButterworthLowpassResp::ButterworthLowpassResp() {
  reset();
}

void ButterworthLowpassResp::reset() {
  x1 = 0.0f;
  x2 = 0.0f;
  y1 = 0.0f;
  y2 = 0.0f;
}

float ButterworthLowpassResp::process(float inSample) {
  float outSample =
      b0 * inSample
    + b1 * x1
    + b2 * x2
    - a1 * y1
    - a2 * y2;

  x2 = x1;
  x1 = inSample;
  y2 = y1;
  y1 = outSample;

  return outSample;
}

// ==========================================
// PPG FILTER
// ==========================================

PPGFilter::PPGFilter() {
  reset();
}

void PPGFilter::reset() {
  dcRed = 0.0f;
  dcIR = 0.0f;
  dcIRResp = 0.0f;

  dcIRRiiv = 0.0f;
  dcEnvelope = 0.0f;

  lastAcIRResp = 0.0f;
  lastEnvelopeLP = 0.0f;
  lastEnvelopeResp = 0.0f;
  lastRiivCandidate = 0.0f;

  filterRed.reset();
  filterIR.reset();
  filterRespIR.reset();
  filterEnvelope.reset();
  filterRiiv.reset();
}

// ==========================================
// MAIN PROCESS
// ==========================================

void PPGFilter::process(
  float rawRed,
  float rawIR,
  float &filteredRed,
  float &filteredIR,
  float &respIR
) {

  // ==========================================
  // 1. HR DAN SpO2
  // ==========================================

  if (dcRed == 0.0f) dcRed = rawRed;
  if (dcIR == 0.0f) dcIR = rawIR;

  dcRed = alphaDC * dcRed
        + (1.0f - alphaDC) * rawRed;

  dcIR = alphaDC * dcIR
       + (1.0f - alphaDC) * rawIR;

  float acRed = rawRed - dcRed;
  float acIR = rawIR - dcIR;

  filteredRed = filterRed.process(acRed);
  filteredIR = filterIR.process(acIR);


  // ==========================================
  // 2. RR LAMA
  // ==========================================

  if (dcIRResp == 0.0f) {
    dcIRResp = rawIR;
  }

  dcIRResp = alphaDCResp * dcIRResp
           + (1.0f - alphaDCResp) * rawIR;

  float acIRResp = rawIR - dcIRResp;

  lastAcIRResp = acIRResp;

  respIR = filterRespIR.process(acIRResp);


  // ==========================================
  // 3. RIAV ENVELOPE LAMA
  // ==========================================

  float rectifiedSignal = fabsf(acIRResp);

  float envelopeLP = filterEnvelope.process(
    rectifiedSignal
  );

  lastEnvelopeLP = envelopeLP;

  if (dcEnvelope == 0.0f) {
    dcEnvelope = envelopeLP;
  }

  dcEnvelope = alphaDCEnvelope * dcEnvelope
             + (1.0f - alphaDCEnvelope) * envelopeLP;

  lastEnvelopeResp = envelopeLP - dcEnvelope;


  // ==========================================
  // 4. KANDIDAT RIIV
  // ==========================================

  if (dcIRRiiv == 0.0f) {
    dcIRRiiv = rawIR;
  }

  // Menghilangkan drift baseline yang sangat lambat
  dcIRRiiv = alphaRiiv * dcIRRiiv
           + (1.0f - alphaRiiv) * rawIR;

  float riivHighpass = rawIR - dcIRRiiv;

  // Lowpass untuk membatasi komponen frekuensi tinggi
  lastRiivCandidate = filterRiiv.process(
    riivHighpass
  );
}