#ifndef PPG_FILTER_H
#define PPG_FILTER_H

class ButterworthBandpass {
private:
  const float b0 = 0.0012224f;
  const float b1 = 0.0f;
  const float b2 = -0.0012224f;
  const float a1 = -1.89063f;
  const float a2 = 0.89307f;
  float x1 = 0.0f, x2 = 0.0f;
  float y1 = 0.0f, y2 = 0.0f;

public:
  ButterworthBandpass();
  float process(float inSample);
  void reset();
};

class ButterworthLowpassResp {
private:
  const float b0 = 0.00000986f;
  const float b1 = 0.00001972f;
  const float b2 = 0.00000986f;
  const float a1 = -1.991114f;
  const float a2 = 0.991154f;
  float x1 = 0.0f, x2 = 0.0f;
  float y1 = 0.0f, y2 = 0.0f;

public:
  ButterworthLowpassResp();
  float process(float inSample);
  void reset();
};

class PPGFilter {
private:
  ButterworthBandpass filterRed;
  ButterworthBandpass filterIR;
  ButterworthLowpassResp filterRespIR;
  float dcRed = 0.0f;
  float dcIR = 0.0f;
  const float alphaDC = 0.995f;

public:
  PPGFilter();
  void process(float rawRed, float rawIR, float &filteredRed,
               float &filteredIR, float &respIR);
  void reset();
  float getDCRed() const { return dcRed; }
  float getDCIR() const { return dcIR; }
};

#endif
