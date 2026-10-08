#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "LowLatConv"
#define DISTRHO_PLUGIN_NAME    "LowLatConv"
#define DISTRHO_PLUGIN_URI     "urn:lowlatconv:lowlatconv"
#define DISTRHO_PLUGIN_CLAP_ID "org.lowlatconv.lowlatconv"
#define DISTRHO_PLUGIN_CLAP_FEATURES "audio-effect", "stereo"

// Interface NanoVG : à activer quand l'UI sera écrite (voir README du dossier plugin).
#define DISTRHO_PLUGIN_HAS_UI          0
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_NUM_INPUTS      2
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_STATE      1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1

#endif
