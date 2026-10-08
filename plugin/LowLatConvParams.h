// Liste des paramètres de LowLatConv, partagée entre le DSP (LowLatConvPlugin.cpp) et l'interface.
// L'indice d'un paramètre est sa valeur dans l'enum Param : c'est celui que DPF utilise partout
// (setParameterValue, parameterChanged, editParameter...).
#ifndef LOWLATCONV_PARAMS_H_INCLUDED
#define LOWLATCONV_PARAMS_H_INCLUDED

#include <cstdint>

namespace LLCParams {

enum Param : uint32_t {
    pInGain = 0, pOutGain, pDry, pWet, pPreDelay, pWidth, pEqOn,
    pB1On, pB1Type, pB1Freq, pB1Gain, pB1Q,
    pB2On, pB2Type, pB2Freq, pB2Gain, pB2Q,
    pB3On, pB3Type, pB3Freq, pB3Gain, pB3Q,
    pHpOn, pHpFreq, pHpSlope, pLpOn, pLpFreq, pLpSlope,
    // Paramètres qui reconstruisent l'IR (non automatisables)
    pInputMode, pStretch, pAutoTrim, pBegin, pEnd, pAutoLen, pAttack, pDecay, pReverse, pNorm,
    // Sorties (affichage)
    pIRLengthMs, pIRStatus,
    // Réglage des vu-mètres (non automatisable)
    pMeterRmsMs,
    // Mesures des vu-mètres (sorties cachées, mises à jour à chaque bloc audio)
    pMInPeakL, pMInPeakR, pMOutPeakL, pMOutPeakR,        // crête sur 100 ms, dBFS
    pMInRmsL, pMInRmsR, pMOutRmsL, pMOutRmsR,            // RMS sur pMeterRmsMs, dB
    pMInLufsM, pMOutLufsM, pMInLufsS, pMOutLufsS,        // LUFS momentané (400 ms) et court terme (3 s), canaux L+R sommés
    // Crête : retombée réglable, remise à zéro, crête maximale exacte (peak watcher)
    pMeterPeakDecay,                                     // réglage, dB/s (non automatisable)
    pMeterReset,                                         // déclencheur : écrire 1 remet à zéro les crêtes max
    pMInPeakMaxL, pMInPeakMaxR, pMOutPeakMaxL, pMOutPeakMaxR,   // sorties : plus grande crête depuis la remise à zéro, dBFS
    pCount
};

static constexpr uint32_t kIRFirst = pInputMode, kIRLast = pNorm;

// Clé de l'état qui contient le chemin du fichier WAV de l'IR (valeur = chemin absolu, "" = aucune IR).
static constexpr const char* kStateIRPath = "ir_path";

enum { kFlagAuto = 1, kFlagLog = 2, kFlagBool = 4, kFlagInt = 8, kFlagOut = 16, kFlagHidden = 32, kFlagTrigger = 64 };
struct ParamDef { const char* name; const char* symbol; const char* unit; float def, min, max; int flags; const char* const* labels; int nLabels; };

static const char* const kTypeLabels[]  = {"Bell", "Low shelf", "High shelf"};
static const char* const kSlopeLabels[] = {"12 dB/oct", "24 dB/oct", "48 dB/oct"};
static const char* const kModeLabels[]  = {"Stereo", "Sum (L+R)", "Left", "Right"};
static const char* const kNormLabels[]  = {"Off", "Peak", "Energy"};
static const char* const kStatusLabels[] = {"No IR", "Loading", "Loaded", "Error"};

#define BAND(n, f) \
    {"EQ" #n " On", "eq" #n "_on", "", 0, 0, 1, kFlagAuto | kFlagBool, nullptr, 0}, \
    {"EQ" #n " Type", "eq" #n "_type", "", 0, 0, 2, kFlagAuto | kFlagInt, kTypeLabels, 3}, \
    {"EQ" #n " Freq", "eq" #n "_freq", "Hz", f, 20, 20000, kFlagAuto | kFlagLog, nullptr, 0}, \
    {"EQ" #n " Gain", "eq" #n "_gain", "dB", 0, -18, 18, kFlagAuto, nullptr, 0}, \
    {"EQ" #n " Q", "eq" #n "_q", "", 1, 0.1f, 10, kFlagAuto | kFlagLog, nullptr, 0}

static const ParamDef kDefs[pCount] = {
    {"Input gain", "in_gain", "dB", 0, -60, 30, kFlagAuto, nullptr, 0},
    {"Output gain", "out_gain", "dB", 0, -60, 30, kFlagAuto, nullptr, 0},
    {"Dry", "dry", "dB", -100, -100, 30, kFlagAuto, nullptr, 0},
    {"Wet", "wet", "dB", 0, -100, 30, kFlagAuto, nullptr, 0},
    {"Pre-delay", "predelay", "ms", 0, 0, 2000, kFlagAuto, nullptr, 0},
    {"Width", "width", "%", 100, 0, 200, kFlagAuto, nullptr, 0},
    {"EQ On", "eq_on", "", 1, 0, 1, kFlagAuto | kFlagBool, nullptr, 0},
    BAND(1, 200), BAND(2, 1000), BAND(3, 4000),
    {"HP On", "hp_on", "", 0, 0, 1, kFlagAuto | kFlagBool, nullptr, 0},
    {"HP Freq", "hp_freq", "Hz", 80, 20, 1000, kFlagAuto | kFlagLog, nullptr, 0},
    {"HP Slope", "hp_slope", "", 1, 0, 2, kFlagAuto | kFlagInt, kSlopeLabels, 3},
    {"LP On", "lp_on", "", 0, 0, 1, kFlagAuto | kFlagBool, nullptr, 0},
    {"LP Freq", "lp_freq", "Hz", 6000, 1000, 20000, kFlagAuto | kFlagLog, nullptr, 0},
    {"LP Slope", "lp_slope", "", 1, 0, 2, kFlagAuto | kFlagInt, kSlopeLabels, 3},
    {"Input mode", "input_mode", "", 1, 0, 3, kFlagInt, kModeLabels, 4},
    {"IR stretch", "ir_stretch", "%", 100, 50, 200, 0, nullptr, 0},
    {"IR auto-trim start", "ir_autotrim", "", 1, 0, 1, kFlagBool, nullptr, 0},
    {"IR begin", "ir_begin", "ms", 0, 0, 500, 0, nullptr, 0},
    {"IR end", "ir_end", "ms", 0, 0, 10000, 0, nullptr, 0},
    {"IR auto-length", "ir_autolen", "", 0, 0, 1, kFlagBool, nullptr, 0},
    {"IR attack", "ir_attack", "ms", 0, 0, 500, 0, nullptr, 0},
    {"IR decay", "ir_decay", "dB", 0, 0, 60, 0, nullptr, 0},
    {"IR reverse", "ir_reverse", "", 0, 0, 1, kFlagBool, nullptr, 0},
    {"IR normalise", "ir_norm", "", 2, 0, 2, kFlagInt, kNormLabels, 3},
    {"IR length", "ir_length", "ms", 0, 0, 20000, kFlagOut, nullptr, 0},
    {"IR status", "ir_status", "", 0, 0, 3, kFlagOut | kFlagInt, kStatusLabels, 4},
    {"Meter RMS window", "meter_rms_ms", "ms", 300, 10, 3000, 0, nullptr, 0},
    {"IN peak L", "m_in_peak_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN peak R", "m_in_peak_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT peak L", "m_out_peak_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT peak R", "m_out_peak_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN RMS L", "m_in_rms_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN RMS R", "m_in_rms_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT RMS L", "m_out_rms_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT RMS R", "m_out_rms_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN LUFS-M", "m_in_lufs_m", "LUFS", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT LUFS-M", "m_out_lufs_m", "LUFS", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN LUFS-S", "m_in_lufs_s", "LUFS", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT LUFS-S", "m_out_lufs_s", "LUFS", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"Meter peak decay", "meter_peak_decay", "dB/s", 24, 5, 300, 0, nullptr, 0},
    {"Meter reset", "meter_reset", "", 0, 0, 1, kFlagBool | kFlagTrigger, nullptr, 0},
    {"IN peak max L", "m_in_peakmax_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"IN peak max R", "m_in_peakmax_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT peak max L", "m_out_peakmax_l", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
    {"OUT peak max R", "m_out_peakmax_r", "dB", -120, -120, 24, kFlagOut | kFlagHidden, nullptr, 0},
};


} // namespace LLCParams

#endif
