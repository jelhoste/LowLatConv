# Moteur de convolution à latence nulle pour guitare (base du futur plugin CLAP / DPF / NanoVG)

Tout est en C++17, en-têtes seuls (sauf PFFFT et dr_wav, fournis dans `third_party/`).
Licences : PFFFT (type BSD), dr_wav (domaine public ou MIT-0).

## Chaîne de traitement (`processor.hpp`, `ConvolutionProcessor`)

    in -> gain d'entrée --+-----------------------------------------------> gain dry --+
                          |                                                           +--> gain de sortie -> out
                          +-> pré-délai -> convolution -> EQ -> largeur -> gain wet --+

Latence ajoutée : 0 échantillon. Sans IR chargée, le plugin est un bypass exact (aux gains près).
Les gains, la largeur et le pré-délai sont lissés (rampes de 20 ms, fondu pour le pré-délai) : pas de clic.
Entrées NaN / Inf neutralisées dès l'entrée de la chaîne.

## Modules
- `convolver.hpp` : moteur à matrice de chemins (n entrées -> m sorties). Tête FIR directe (échantillon par
  échantillon) + partitions FFT non uniformes (schéma de Gardner). Partage des FFT d'entrée et de sortie entre
  chemins. Accepte toute taille de bloc hôte.
- `stereo_engine.hpp` : routage d'une IR de 1, 2 ou 4 canaux (mono / stéréo / vrai stéréo) selon le mode
  d'entrée (Stereo, Sum, Left, Right). Voir le tableau en tête du fichier.
- `live_convolver.hpp` : changement d'IR à chaud sans clic (fondu de 1024 échantillons), libération des anciens
  moteurs hors du thread audio.
- `ir_processor.hpp` : préparation de l'IR hors thread audio (conversion de fréquence d'échantillonnage et
  étirement par sinc fenêtré, rognage auto du silence initial, début/fin, longueur auto, fondus de coupe,
  attaque/décroissance, inversion, normalisation pic ou énergie).
- `wav_loader.hpp` : lecture des WAV d'IR (PCM 8/16/24/32, flottant 32/64, extensible, Wave64) avec limites de
  taille et de canaux, fichiers tronqués acceptés, entrées corrompues refusées proprement.
- `eq.hpp` : 3 bandes paramétriques (cloche, shelf grave, shelf aigu) + passe-haut et passe-bas Butterworth
  12/24/48 dB/oct, double précision, paramètres lissés.
- `ir_display.hpp` : données d'affichage de l'IR pour l'interface (courbe min/max, spectre en dB, thread de calcul).
- `meters.hpp` : vu-mètres (crête à attaque instantanée, maintien 50 ms et retombée en dB/s réglable, crête maximale
  exacte avec remise à zéro, RMS à fenêtre réglable, LUFS-M et LUFS-S selon l'UIT BS.1770).
- `fft_backend.hpp`, `fft.hpp` : choix de la FFT (`-DLLC_USE_PFFFT` recommandé ; sinon FFT radix-2 intégrée).
- `fonts/` : police Inter (Regular, SemiBold, Bold), licence SIL OFL 1.1 (`OFL-Inter.txt`, à distribuer avec le plugin). `tools/embed_font.py` les convertit en en-tête C++ pour les embarquer dans le binaire.
- `denormals.hpp` : désactivation des dénormaux (x86 testé, AArch64 non testé).

## Utilisation (esquisse)

    // thread de chargement (jamais le thread audio)
    llc::IRBuffer raw;  llc::loadWavFile(path, raw);
    llc::IRBuffer ir;   llc::processIR(raw, options /* targetSampleRate = fréquence de la session */, ir);
    proc.loadEngine(llc::StereoEngine::build(ir, llc::InputMode::Sum));
    proc.collectRetired();

    // thread audio
    proc.setParams(params);                       // depuis la file de paramètres
    proc.process(inL, inR, outL, outR, n);        // in == out autorisé

Les retouches d'IR (rognage, étirement, enveloppe, inversion, normalisation) et le mode d'entrée demandent de
reconstruire l'IR : à déclarer non automatisables dans le plugin. Tout le reste est automatisable.

## Plugin
Le dossier `plugin/` contient le plugin DPF (CLAP vérifié, sans interface pour l'instant) et un hôte CLAP de
test. Voir `plugin/README.md`.

## Compiler et tester
    make -f Makefile.tests check      # 12 jeux de tests : FFT, moteur, routage, changement d'IR, EQ, IR, affichage, vu-mètres, WAV, chaîne
    make -f Makefile.tests OPT="-O3 -march=native" bench   # mesures de charge (les deux FFT)
    make -f Makefile.tests sanitize   # ASan + UBSan sur tout, TSan sur les tests à threads (long : lancer en arrière-plan)

Le plugin (nécessite DPF dans `dpf/`, voir `docs/CI.md`) : `make` à la racine -> `bin/LowLatConv.clap`.
Compilation Linux / Windows / macOS sans machine locale : `docs/CI.md` (GitHub Actions).
Outils de vérification de l'interface sans OpenGL : `ui_tools/`.

Pour un plugin distribué, ne pas compiler avec `-march=native` (c'est le cas par défaut) : base x86-64 (SSE2) / ARM64.

## Limites connues
- Pas encore écrits : l'interface NanoVG (et donc le sélecteur de fichier d'IR), le mode « économie » avec
  latence déclarée.
- La décroissance d'IR ne fait que raccourcir la queue.
- Changer la pente du passe-haut/bas pendant la lecture remet son état à zéro (petit transitoire possible).
- Code testé uniquement sur x86-64 avec g++ sous Linux. Windows, macOS et ARM non testés.
- Les mesures de charge viennent d'un cœur unique partagé : à refaire sur ta machine.
