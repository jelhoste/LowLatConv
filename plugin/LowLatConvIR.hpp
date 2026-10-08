// Conversion « valeurs des paramètres -> options de préparation de l'IR », partagée par le DSP
// (thread de chargement) et par l'interface (affichage de l'IR) : les deux calculent exactement la même IR.
// `v` est un tableau de pCount valeurs indexé par LLCParams::Param.
#ifndef LOWLATCONV_IR_HPP_INCLUDED
#define LOWLATCONV_IR_HPP_INCLUDED

#include "LowLatConvParams.h"
#include "ir_processor.hpp"
#include "stereo_engine.hpp"

namespace LLCParams {

inline llc::InputMode makeInputMode(const float* v) {
    const int m = int(v[pInputMode] + 0.5f);
    return llc::InputMode(m < 0 ? 0 : (m > 3 ? 3 : m));
}

inline llc::IROptions makeIROptions(const float* v, double sampleRate) {
    llc::IROptions o;
    o.targetSampleRate = sampleRate;
    o.stretch = v[pStretch] / 100.0;
    o.autoTrimStart = v[pAutoTrim] > 0.5f;
    o.beginMs = v[pBegin]; o.endMs = v[pEnd];
    o.autoLength = v[pAutoLen] > 0.5f;
    o.attackMs = v[pAttack]; o.decayDb = v[pDecay];
    o.reverse = v[pReverse] > 0.5f;
    const int n = int(v[pNorm] + 0.5f);
    o.norm = llc::NormMode(n < 0 ? 0 : (n > 2 ? 2 : n));
    return o;
}

} // namespace LLCParams

#endif
