# Plugin DPF « LowLatConv » (CLAP, VST3, LV2... — sans interface pour l'instant)

## Compiler
1. DPF (https://github.com/DISTRHO/DPF) en sous-module Git dans `../dpf` (voir `../docs/CI.md`),
   ou passer `DPF_PATH=/chemin/vers/DPF` à make.
2. Depuis la racine du dépôt : `make` -> `bin/LowLatConv.clap` (ou, dans ce dossier, `make clap`).
   Les autres formats passent par le même Makefile (`make vst3`, `make lv2_sep`...), mais seul CLAP a été
   compilé et testé ici.
3. Test de bout en bout avec un hôte CLAP minimal (charge le .clap, pilote l'API CLAP, vérifie l'audio) :

       g++ -std=c++17 clap_host_test.cpp -I<DPF>/distrho/src -ldl -pthread -o clap_host_test
       ./clap_host_test bin/LowLatConv.clap

Pour un plugin distribué : ne pas utiliser `-march=native` (la base x86-64 SSE2 suffit).

## Ce que fait le plugin
- Entrées / sorties : stéréo (1 port stéréo en entrée, 1 en sortie). Latence ajoutée : 0.
- État : clé `ir_path` (chemin absolu du fichier WAV, sauvegardé avec le projet).
- 59 paramètres : 28 automatisables (gains, dry/wet, pré-délai, largeur, EQ 3 bandes, passe-haut, passe-bas),
  10 non automatisables qui reconstruisent l'IR (mode d'entrée, étirement, rognage auto, début, fin, longueur
  auto, attaque, décroissance, inversion, normalisation), 2 réglages de vu-mètres (fenêtre RMS, retombée de la crête), 1 déclencheur
  (remise à zéro des crêtes max) et 18 sorties (longueur et statut de l'IR, 16 mesures : crête, crête max,
  RMS, LUFS-M, LUFS-S).
- Un thread de chargement lit le WAV, prépare l'IR et construit le moteur (débounce de 60 ms), puis le
  thread audio bascule dessus par fondu. Le thread audio n'alloue, ne verrouille et ne libère jamais rien.
- Statut IR : 0 aucune, 1 chargement, 2 chargée, 3 erreur.

## Fichiers partagés avec l'interface
`LowLatConvParams.h` (enum, plages, clé du state), `LowLatConvIR.hpp` (valeurs des paramètres -> options d'IR),
et, à la racine, `ir_display.hpp` (données de courbe et de spectre de l'IR). Voir
`../NOVA_LowLatConv_BRIEF_DSP_reponses.md` pour le détail des échanges DSP <-> UI.

## Interface (à venir)
Dans `DistrhoPluginInfo.h` : passer `DISTRHO_PLUGIN_HAS_UI` à 1, ajouter `DISTRHO_UI_USE_NANOVG 1` et
`DISTRHO_UI_FILE_BROWSER 1` (attention : `requestStateFile` n'est pas implémenté dans le wrapper CLAP de DPF,
utiliser `openFileBrowser` + `uiFileBrowserSelected`), puis ajouter les fichiers d'UI à `FILES_UI` dans le Makefile. La compilation de
l'UI demande les en-têtes X11 et OpenGL sous Linux.

## À décider
Nom du plugin, identifiants (`DISTRHO_PLUGIN_URI`, `DISTRHO_PLUGIN_CLAP_ID`, identifiant unique), licence
déclarée dans `getLicense()`.
