#ifndef PPG_FILTER_H
#define PPG_FILTER_H

class ButterworthBandpass {
private:
  const float b0 =  0.03415017f;
  const float b1 =  0.0f;
  const float b2 = -0.03415017f;
  const float a1 = -1.93110366f;
  const float a2 =  0.93169966f;

  float x1 = 0.0f, x2 = 0.0f;
  float y1 = 0.0f, y2 = 0.0f;

public:
  ButterworthBandpass();
  float process(float inSample);
  void reset();
};

class ButterworthLowpassResp {
private:
  const float b0 = 0.000087655549f;
  const float b1 = 0.000175311098f;
  const float b2 = 0.000087655549f;
  const float a1 = -1.973344250f;
  const float a2 = 0.973694872f;

  float x1 = 0.0f, x2 = 0.0f;
  float y1 = 0.0f, y2 = 0.0f;

public:
  ButterworthLowpassResp();
  float process(float inSample);
  void reset();
};

class PPGFilter {
private:
  // HR dan SpO2
  ButterworthBandpass filterRed;
  ButterworthBandpass filterIR;

  // RR lama
  ButterworthLowpassResp filterRespIR;

  // Envelope RIAV
  ButterworthLowpassResp filterEnvelope;

  // Kandidat jalur RIIV
  ButterworthLowpassResp filterRiiv;

  float dcRed = 0.0f;
  float dcIR = 0.0f;
  float dcIRResp = 0.0f;

  const float alphaDC = 0.995f;
  const float alphaDCResp = 0.9992149f;

  // Baseline khusus RIIV
  float dcIRRiiv = 0.0f;
  const float alphaRiiv = 0.999529f;

  // Baseline envelope
  float dcEnvelope = 0.0f;
  const float alphaDCEnvelope = 0.9992149f;

  // Monitoring
  float lastAcIRResp = 0.0f;
  float lastEnvelopeLP = 0.0f;
  float lastEnvelopeResp = 0.0f;
  float lastRiivCandidate = 0.0f;

public:
  PPGFilter();

  void process(
    float rawRed,
    float rawIR,
    float &filteredRed,
    float &filteredIR,
    float &respIR
  );

  void reset();

  float getDCRed() const {
    return dcRed;
  }

  float getDCIR() const {
    return dcIR;
  }

  float getDCIRResp() const {
    return dcIRResp;
  }

  float getAcIRResp() const {
    return lastAcIRResp;
  }

  float getEnvelopeLP() const {
    return lastEnvelopeLP;
  }

  float getEnvelopeResp() const {
    return lastEnvelopeResp;
  }

  float getRiivCandidate() const {
    return lastRiivCandidate;
  }
};

#endif