#ifndef PPG_VIEW_H
#define PPG_VIEW_H

#include <Arduino.h>

bool ppgBegin();
bool ppgUpdate();
bool ppgConnected();
bool ppgFingerDetected();
float ppgGetBPM();
float ppgGetSpO2();
float ppgGetRR();

#endif