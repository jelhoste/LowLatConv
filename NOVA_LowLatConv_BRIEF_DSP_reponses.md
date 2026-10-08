# NOVA LowLatConv — réponses au brief DSP

Version du DSP décrite ici : celle de l'archive `lowlat_conv.tar.gz` livrée avec ce document.
Tout ce qui est écrit « vérifié » a été exécuté ou lu dans les sources de DPF fournies ; ce qui n'a pas pu être vérifié est dit explicitement.
**Les éléments d'interface ne sont pas compilables dans mon environnement** (pas d'en-têtes X11 / OpenGL) : les extraits côté UI sont des esquisses non compilées.

---

## 0. Ce qui a changé dans le DSP à cause de ce brief

| Changement | Pourquoi |
|---|---|
| Le state s'appelle maintenant **`"ir_path"`** (constante `LLCParams::kStateIRPath`) | décision du brief |
| Un chargement **périmé est abandonné** : si un nouveau chemin ou réglage arrive pendant un chargement, l'ancien résultat n'est jamais installé | avant, l'ancienne IR était installée puis remplacée |
| En cas d'erreur (fichier introuvable, WAV invalide, IR vide après retouches) **l'IR est déchargée** et le plugin repasse en bypass | cohérence avec le chemin sauvegardé |
| **Début / Fin** (`pBegin`, `pEnd`) sont tous deux mesurés sur l'axe de temps de l'IR *après rognage automatique* (0 ms = première trame utile) | avant, `pEnd` était absolu et ambigu |
| **Vu-mètres ajoutés** : réglages (`pMeterRmsMs`, `pMeterPeakDecay`), déclencheur `pMeterReset` et 16 sorties (crête, crête max, RMS, LUFS-M, LUFS-S) | section 5 |
| Nouveaux fichiers partagés DSP / UI : `LowLatConvParams.h` (enum, plages, clé du state), `LowLatConvIR.hpp` (valeurs des paramètres → options d'IR), `ir_display.hpp` (données d'affichage de l'IR) | sections 2 et 3 |

Le nombre de paramètres passe de 40 à **59**. Les indices 0 à 39 ne changent pas ; les nouveaux sont à la fin.

---

## 1. Gestion du chemin du fichier IR (state)

### a) Déclaration (`initState`)
```cpp
void initState(uint32_t, State& s) override
{
    s.hints = kStateIsFilenamePath;
    s.key = kStateIRPath;            // "ir_path"
    s.label = "Impulse response";
    s.defaultValue = "";
}
```
Dans `DistrhoPluginInfo.h` : `DISTRHO_PLUGIN_WANT_STATE 1` et `DISTRHO_PLUGIN_WANT_FULL_STATE 1` sont déjà en place. Pour l'UI il faudra ajouter `DISTRHO_UI_FILE_BROWSER 1`.

### b) `setState` / `getState`
```cpp
void setState(const char* key, const char* value) override
{
    if (std::strcmp(key, kStateIRPath) != 0) return;
    { std::lock_guard<std::mutex> lk(pathMutex_); irPath_ = value ? value : ""; }
    irStamp_.fetch_add(1, std::memory_order_relaxed);   // demande de (re)chargement
}
String getState(const char* key) const override
{
    if (std::strcmp(key, kStateIRPath) != 0) return String();
    std::lock_guard<std::mutex> lk(pathMutex_);
    return String(irPath_.c_str());
}
```
`setState` ne fait que mémoriser le chemin et lever un drapeau : le chargement n'a pas lieu dans cet appel.

### ⚠ Point important pour l'UI : `requestStateFile()` ne fonctionne pas en CLAP dans ce DPF
Dans `DistrhoPluginCLAP.cpp` de l'archive fournie, le rappel de requête de fichier est **non implémenté** (`nullptr, // TODO fileRequestCallback`). `requestStateFile("ir_path")` n'ouvrira donc rien sous CLAP.

**Solution (API vérifiée dans `DistrhoUI.hpp`)** : utiliser le sélecteur natif de la fenêtre, puis envoyer le chemin au DSP soi-même.
```cpp
// UI : DistrhoPluginInfo.h doit contenir  #define DISTRHO_UI_FILE_BROWSER 1
void openIRBrowser()
{
    FileBrowserOptions o;                      // champs : saving, defaultName, startDir, title, className, buttons
    o.title = "Choose an impulse response (.wav)";
    o.startDir = fLastDir.c_str();             // optionnel : dernier dossier utilisé
    openFileBrowser(o);
}
void uiFileBrowserSelected(const char* filename) override   // nullptr / vide si annulé : à ignorer
{
    if (filename == nullptr || filename[0] == '\0') return;
    fIRPath = filename;
    setState(LLCParams::kStateIRPath, filename);            // envoie le chemin au DSP
    requestDisplayUpdate();                                 // voir section 2
}
void stateChanged(const char* key, const char* value) override
{   // appelé à l'ouverture de l'UI avec le chemin sauvegardé (vérifié dans le wrapper CLAP), et pour les changements venant de l'hôte
    if (std::strcmp(key, LLCParams::kStateIRPath) == 0) { fIRPath = value; requestDisplayUpdate(); }
}
```
Le sélecteur ne filtre pas les extensions : l'UI doit afficher l'erreur si le fichier n'est pas un WAV (statut 3).

### c) Comment le chargement est lancé
- **Thread dédié** (créé dans le constructeur du plugin, arrêté dans le destructeur). Le thread audio ne lit, n'alloue et ne charge jamais rien.
- Le thread regarde un compteur toutes les 20 ms. Quand le compteur change (nouveau chemin **ou** changement d'un paramètre d'IR), il attend 60 ms sans nouvelle modification (**débounce**), puis lit le WAV, prépare l'IR, construit le moteur et le fait basculer.
- **Nouveau chemin pendant un chargement** : le résultat en cours est **abandonné** (jamais installé) et le thread repart avec la demande la plus récente. Vérifié par lecture du code ; pas de test automatique dédié à cette course.
- **Chemin absolu**, stocké tel que l'UI ou l'hôte l'a donné. Aucune conversion en chemin relatif au projet. Si le fichier a disparu au rechargement d'un projet : statut 3 (erreur), le chemin reste dans le state (l'UI peut l'afficher en rouge), l'audio est en bypass.
- Le WAV brut est conservé en mémoire : changer un réglage d'IR sans changer le chemin ne relit pas le fichier.

### d) `pIRStatus` et `pIRLengthMs`
| Moment | `pIRStatus` | `pIRLengthMs` |
|---|---|---|
| chemin vide | 0 (No IR) | 0 |
| début de traitement (après débounce) | 1 (Loading) | 0 |
| IR installée | 2 (Loaded) | durée réelle de l'IR **après transformations**, en ms |
| fichier illisible, WAV invalide, IR vide après retouches | 3 (Error) | 0 |

Ce sont des **paramètres de sortie** : le wrapper CLAP les relit à la fin de chaque appel `process` et à chaque appel `params.flush` de l'hôte (vérifié dans `DistrhoPluginCLAP.cpp`), puis prévient l'UI. **Limite** : si l'hôte n'appelle ni `process` ni `flush` (plugin suspendu), l'UI ne reçoit pas les changements ; le comportement dépend de l'hôte et je ne l'ai pas testé sur de vrais hôtes. L'UI qui charge elle-même le fichier pour l'affichage (section 2) connaît de toute façon le résultat.

### e) Formats acceptés
- **WAV uniquement** (via dr_wav, domaine public). FLAC non pris en charge (il faudrait ajouter `dr_flac.h`, de la même famille : je peux le faire).
- Codages : PCM 8 / 16 / 24 / 32 bits, flottant 32 / 64 bits, WAVE_FORMAT_EXTENSIBLE, Wave64 (et d'autres, ADPCM, A-law, µ-law, gérés par dr_wav mais non testés par moi). Fichier tronqué accepté jusqu'à la dernière trame complète.
- **Canaux** : jusqu'à 8 acceptés à la lecture. Utilisés : 1 (mono), 2 (stéréo), 4 (vrai stéréo, ordre LL, LR, RL, RR). 3 canaux → les 2 premiers ; plus de 4 → les 4 premiers.
- **Fréquence d'échantillonnage** : de 1 kHz à 768 kHz acceptées. **Rééchantillonnée vers la fréquence de l'hôte** par sinc fenêtré (Kaiser), dans les deux sens, sans perte audible (erreur mesurée à -111 dB). L'IR est refaite si la fréquence de l'hôte change.
- **Limites** : fichier ≤ 256 Mo, ≤ 16 M de trames ; IR finale plafonnée à 1 048 576 échantillons (≈ 21,8 s à 48 kHz).

---

## 2. Données de l'IR pour l'affichage

### Décision proposée : l'UI calcule l'affichage elle-même, avec le même code que le DSP
**Pourquoi pas un state en lecture seule ?** Vérifié dans `DistrhoPluginCLAP.cpp` : `updateStateValue()` (le mécanisme prévu pour pousser un state du DSP vers l'UI) est un **stub** sous CLAP (`bool updateState(const char*, const char*) { return true; }`) : l'UI ne serait jamais prévenue. Les paramètres de sortie ne conviennent pas à des tableaux de 800 points.

**Ce qui est fourni et testé** (`ir_display.hpp`, `LowLatConvIR.hpp`) :
- `computeIRDisplay(ir, inputMode)` : WAVE = min / max par colonne (800 par défaut), SPECTRUM = module en dB sur axe logarithmique (256 points, 20 Hz à min(20 kHz, 0,45·fs), plancher -120 dB), pour 1 ou 2 traces (L, R).
- `LLCParams::makeIROptions(valeurs, fréquence)` et `makeInputMode(valeurs)` : **le DSP et l'UI les utilisent tous les deux** → exactement la même IR, les mêmes transformations (rognage, étirement, attaque, décroissance, inversion, normalisation, mode d'entrée).
- `IRDisplayWorker` : thread de fond côté UI. `request(tâche)` depuis l'UI, `fetch(sortie, dernièreVersion)` appelé dans le rappel d'inactivité. Une demande plus récente remplace une demande en cours.

```cpp
// UI : membres
llc::IRDisplayWorker fDisplayWorker;  llc::IRDisplay fDisplay;  uint32_t fDisplayVersion = 0;
float fValues[LLCParams::pCount];  std::string fIRPath;     // fValues mis à jour dans parameterChanged()

void requestDisplayUpdate()
{
    std::array<float, LLCParams::pCount> v; std::copy(fValues, fValues + LLCParams::pCount, v.begin());
    const std::string path = fIRPath; const double sr = getSampleRate();
    fDisplayWorker.request([v, path, sr](llc::IRBuffer& ir, llc::InputMode& mode) {
        llc::IRBuffer raw;
        if (path.empty() || llc::loadWavFile(path, raw) != llc::WavStatus::Ok) return false;
        if (!llc::processIR(raw, LLCParams::makeIROptions(v.data(), sr), ir)) return false;
        mode = LLCParams::makeInputMode(v.data());  return true; });
}
void uiIdle() override { if (fDisplayWorker.fetch(fDisplay, fDisplayVersion)) { fDisplayVersion = fDisplay.version; repaint(); } }
```
Appeler `requestDisplayUpdate()` quand le chemin change **et** quand l'un des paramètres `pInputMode … pNorm` change (les valeurs appliquées au relâchement suffisent).

### Réponses aux sous-questions
- **a)** Calculées côté **DSP/chargement** : non possible vers l'UI sous CLAP (voir plus haut). Calculées **côté UI avec le même code** : oui. Durée typique : quelques ms à quelques dizaines de ms (lecture + préparation de l'IR + FFT du spectre) ; le thread de fond de l'UI évite tout blocage de l'interface.
- **b)** Transmission : aucune transmission. Taille maximale : sans objet. Un `IRDisplay` pèse environ 800×4×2×2 + 256×4×2 ≈ 15 Ko en mémoire.
- **c)** L'UI sait qu'une nouvelle version est prête grâce au champ `IRDisplay::version` (incrémenté à chaque résultat) : `fetch()` renvoie `true` seulement si la version a changé.
- **d)** Durée et longueur réelles : `pIRLengthMs` (DSP, autoritaire) suffit pour l'affichage de la durée ; `IRDisplay` contient aussi `durationMs`, `lengthSamples`, `peak`, `fMin`, `fMax`.

### Quelle trace est dessinée (important pour le mode d'entrée)
| IR | Trace L | Trace R |
|---|---|---|
| 1 canal | h0 | h0 (identique) |
| 2 canaux | h0 | h1 |
| 4 canaux, Stereo | h0 (L→L) | h3 (R→R) |
| 4 canaux, Sum | h0 + h2 | h1 + h3 |
| 4 canaux, Left | h0 | h1 |
| 4 canaux, Right | h2 | h3 |

Les chemins croisés d'un vrai stéréo en mode Stereo (L→R, R→L) ne sont pas dessinés.

### Dépendances à prévoir côté UI
- Inclure `ir_display.hpp`, `wav_loader.hpp`, `LowLatConvIR.hpp` (même `-DLLC_USE_PFFFT` et mêmes chemins d'inclusion que le DSP).
- **Piège de liaison** : sous CLAP / VST3, DSP et UI sont dans le même binaire. `dr_wav_impl.cpp` ne doit être compilé **qu'une fois** (dans `FILES_DSP`), sinon « multiple definition » (vérifié). Sous LV2 (binaire d'UI séparé), il faut l'ajouter aussi à `FILES_UI`.

---

## 3. Paramètres qui reconstruisent l'IR (non automatisables)

- **a)** Quand l'un de `pInputMode, pStretch, pAutoTrim, pBegin, pEnd, pAutoLen, pAttack, pDecay, pReverse, pNorm` change : **reconstruction complète** de l'IR **dans le thread de chargement**, après 60 ms sans nouveau changement. Le fichier n'est pas relu. Durées mesurées dans mon environnement (cœur partagé, à titre indicatif) :

| IR source | Préparation de l'IR | Construction du moteur |
|---|---|---|
| mono 0,5 s, 48 kHz | 0,1 ms | 0,2 ms |
| stéréo 1 s, 44,1 kHz → 48 kHz | 18 ms | 0,7 ms |
| 4 canaux 2 s, 48 kHz | 1,2 ms | 2,5 ms |
| stéréo 5 s, 96 kHz → 48 kHz | 163 ms | 3,4 ms |

  (La lecture du WAV est de 0,1 à 3 ms en plus quand le fichier change.) Le coût est surtout celui du rééchantillonnage (fréquence ≠ hôte ou étirement ≠ 100 %).
- **b)** Appliquer la valeur **au relâchement** est compatible : `setParameterValue(index, valeur)` une seule fois suffit. Appliquer en continu l'est aussi (le débounce de 60 ms regroupe les changements). Plusieurs paramètres d'IR modifiés dans un court intervalle déclenchent **une seule** reconstruction.
- **c)** Pendant la reconstruction l'audio **continue avec l'ancienne IR** (aucun trou). Le nouveau moteur est installé par un **fondu enchaîné de 1024 échantillons** (≈ 21 ms à 48 kHz). Si une autre IR arrive pendant un fondu, elle attend la fin du fondu.
- **d)** `pAutoTrim` / `pAutoLen` **n'ignorent pas** `pBegin` / `pEnd` : ils se combinent. Ordre exact : étirement → rognage automatique (le silence initial) → `pBegin` (décalage mesuré depuis la première trame utile) → `pEnd` (position de coupe sur le même axe, 0 = pas de coupe) → longueur auto (ne peut que raccourcir encore). **Il ne faut donc pas griser Begin / End** quand l'automatique est actif. `pBegin` et `pEnd` sont en ms sur l'axe de l'IR déjà étirée.

---

## 4. Chaîne de traitement (ordre exact)

```
entrée → InGain ─┬────────────────────────────────────────────→ ×Dry ─┐
                 │                                                      ├→ OutGain → sortie
                 └→ PreDelay → convolution → EQ → Width → ×Wet ────────┘
EQ = passe-haut → bande 1 → bande 2 → bande 3 → passe-bas
```
- **a)** L'EQ s'applique **uniquement au wet**. ✔
- **b)** `pPreDelay` et `pWidth` : **wet seulement**. Le dry n'est touché que par InGain et OutGain.
- **c)** **Oui** : pour `pDry` et `pWet`, toute valeur ≤ -100 dB donne un gain **exactement nul** (−∞). Afficher « -inf » quand la valeur est ≤ -100. Exception : **sans IR chargée**, le plugin est un bypass : le dry est forcé à 0 dB si `pDry` est à −∞ (le signal passe) ; si `pDry` est réglé plus haut, c'est sa valeur qui s'applique.
- **d)** Latence reportée à l'hôte : **0 échantillon**, constante : elle ne dépend d'aucun paramètre (taille de partition, IR, mode d'entrée). Le plugin n'appelle pas `setLatency`. **Le pré-délai n'est pas de la latence** : c'est un retard voulu, non compensé par l'hôte (le wet est retardé par rapport au dry). Pour la barre du bas : afficher « Latency 0 smp », éventuellement « + pré-délai X ms (wet) ».
  Ne comprend pas la latence de l'interface audio ni du tampon de l'hôte.

---

## 5. Vu-mètres

- **a)** Avant ce brief : **rien n'était mesuré**. C'est maintenant fait côté DSP (`meters.hpp`, testé) :
  - **IN** = signal **après le gain d'entrée** (ce qui entre dans la chaîne ; utile pour régler le gain). Si tu préfères IN avant le gain, c'est une ligne à changer : dis-le.
  - **OUT** = sortie finale, **après le gain de sortie**.
  - L et R séparés pour crête et RMS ; LUFS sur L+R sommés.
- **b)** Transmission : **sorties DPF cachées en lecture seule**, mises à jour à chaque bloc audio (le wrapper CLAP les transmet à l'UI à son rythme ; viser 30 à 60 Hz côté UI est sans problème). Les mesures sont intégrées côté DSP : l'UI n'a rien à intégrer, seulement à afficher et lisser.
  Remarque vérifiée : le wrapper CLAP **ne transmet pas** l'indicateur « caché » à l'hôte ; ces paramètres apparaissent donc en lecture seule dans la liste de l'hôte. Même limite que pour `pIRStatus` si l'hôte n'appelle ni `process` ni `flush`.
- **c)** Réparti ainsi :

| Mode de l'UI | Calculé côté DSP | Paramètres |
|---|---|---|
| Peak | **crête avec attaque instantanée, maintien 50 ms, puis retombée linéaire en dB/s réglable** (voir ci-dessous), dBFS | `pMInPeakL/R`, `pMOutPeakL/R`, retombée = `pMeterPeakDecay` |
| Peak hold / Peak watcher | **crête maximale exacte depuis la dernière remise à zéro**, dBFS | `pMInPeakMaxL/R`, `pMOutPeakMaxL/R`, remise à zéro = `pMeterReset` |
| RMS / profils RMS personnalisés | moyenne quadratique sur une fenêtre **réglable de 10 à 3000 ms** (défaut 300 ms), en dB (une sinusoïde pleine échelle lit −3,01) | `pMInRmsL/R`, `pMOutRmsL/R`, fenêtre = `pMeterRmsMs` |
| VU | **à faire côté UI** : prendre le RMS avec `pMeterRmsMs` = 300 ms et appliquer la balistique d'affichage | idem |
| EBU-M | **LUFS momentané** (pondération K, ITU-R BS.1770, fenêtre 400 ms, L+R sommés) | `pMInLufsM`, `pMOutLufsM` |
| EBU-S | **LUFS court terme** (3 s) | `pMInLufsS`, `pMOutLufsS` |

  Vérifié : une sinusoïde 1 kHz à −23 dBFS sur les deux canaux donne −23,0 LUFS (référence EBU Tech 3341) à 44,1 / 48 / 96 kHz ; une sinusoïde à −20 dBFS donne crête −20,00 dB, RMS −23,01 dB. Les coefficients de la pondération K sont ceux publiés par l'UIT pour 48 kHz.
  Plancher de toutes les valeurs : **−120**. Pas de LRA. Pas de crête vraie (true peak).
### Comportement de la crête (correction par rapport à ma première version)
Ta remarque était juste : ma première version renvoyait le maximum des 100 dernières ms, c'est-à-dire un **palier de 100 ms puis une chute en marche d'escalier** — ni instantané au sens strict, ni une vraie retombée. C'est remplacé par :

| Étape | Comportement | Vérifié |
|---|---|---|
| Attaque | **instantanée** : la crête est rapportée par le bloc audio qui la contient, **quelle que soit la taille du bloc** (1 à 9000 échantillons testés), erreur < 0,01 dB | oui, test automatique |
| Maintien | la valeur reste au niveau de la crête pendant **50 ms** (plus long qu'un rafraîchissement d'UI à 20 Hz ou plus : aucune crête ne peut passer entre deux images) | oui |
| Retombée | **linéaire en dB**, vitesse réglable avec `pMeterPeakDecay` : 5 à 300 dB/s, **défaut 24 dB/s** (entre 40 dB/s, type REAPER, et 13,3 dB/s, norme) | oui : pente par défaut mesurée −24 dB après 1 s de retombée ; pente à 40 dB/s mesurée −40,0 ±0,5 dB/s ; à 200 dB/s : −40 dB après 200 ms |
| Une crête plus petite | ne fait jamais baisser l'enveloppe ; une crête plus grande la relève instantanément | oui |
| Plancher | −120 dB | oui |

Valeurs mesurées pour une crête à 0 dBFS (dB, après 0 / 20 / 50 / 100 / 250 / 500 / 1000 ms) :
- 13,3 dB/s : 0,00 / 0,00 / 0,00 / −0,65 / −2,65 / −5,97 / −12,62
- **24 dB/s (défaut)** : 0,00 / 0,00 / 0,00 / −1,18 / −4,78 / −10,78 / −22,78
- 40 dB/s : 0,00 / 0,00 / 0,00 / −1,96 / −7,96 / −17,96 / −37,96
- 60 dB/s : 0,00 / 0,00 / 0,00 / −2,94 / −11,94 / −26,94 / −56,94

C'est une crête d'**échantillon** (pas de crête vraie / true peak).

**Peak hold et peak watcher.** Tu as déjà ces éléments dans l'UI ; le DSP fournit maintenant la donnée exacte pour ne pas dépendre du rythme de rafraîchissement :
- `pMInPeakMaxL/R`, `pMOutPeakMaxL/R` : plus grande valeur absolue d'échantillon depuis la remise à zéro, **exacte à l'échantillon près** (testé : un seul échantillon à 0,5 donne −6,021 dB), indépendante de la retombée.
- `pMeterReset` : paramètre **déclencheur**. L'UI fait `setParameterValue(pMeterReset, 1.0f)` ; le DSP remet les crêtes max à zéro et **ramène lui-même le paramètre à 0** (DPF ne le réarme pas : vérifié dans les sources). Testé de bout en bout avec l'hôte CLAP de test.
- Deux usages possibles du côté UI : (1) marqueur de maintien dessiné par l'UI à partir de `pMInPeakL` (maintien + chute de son choix) ; (2) lecture numérique « crête max » = `pMInPeakMax*`, remise à zéro par clic sur l'affichage.
- **Pour éviter une double ballistique** : si l'UI applique sa propre retombée, mettre `pMeterPeakDecay` à 300 (la valeur du DSP devient presque brute, avec ses 50 ms de maintien) ; sinon laisser le défaut (24 dB/s) et dessiner la valeur telle quelle.

### Ce que j'ai trouvé sur REAPER et les autres logiciels
Sources consultées : le panneau « Track Control Panels » de REAPER tel que décrit par deux utilisateurs, le wiki d'accessibilité de REAPER sur le Peak Watcher, et la documentation de plusieurs mètres (GoldWave, Sound Forge, Sound Devices, Biamp).
- **REAPER** propose dans Preferences > Appearance > Track Control Panels : *Meter update frequency (Hz)*, *Meter minimum value (dB)* et **Meter decay (dB/sec)**. Les réglages publiés par des utilisateurs sont **60 dB/s** (avec 60 Hz de rafraîchissement) et **40 dB/s** (avec 30 Hz, conseillé pour des mètres plus lisses). **Je n'ai pas trouvé la valeur par défaut de REAPER ni le détail exact de son algorithme** : je ne les affirme donc pas. Mon modèle (attaque instantanée, retombée en dB/s) en reprend le principe.
- **Peak Watcher de REAPER** : surveille une ou deux pistes, avec « hold peaks until reset » (conserver la crête maximale jusqu'à remise à zéro, réglage par défaut) ou pour une durée donnée, une remise à zéro manuelle et une alerte à un seuil choisi. C'est exactement le rôle de `pMInPeakMax*` + `pMeterReset`. L'alerte à seuil est à faire dans l'UI (comparer `pMInPeakMax*` au seuil).
- **Autres logiciels** : GoldWave et Sound Forge proposent aussi un *Peak hold time*, un *Decay time*, un indicateur de saturation remis à zéro par un clic. Sound Devices annonce une attaque de 0,1 ms et une retombée de 100 ms de constante de temps pour ses LEDs de crête.
- **Normes** (de mémoire, non revérifiées dans cette session) : les PPM de la CEI 60268-10 retombent d'environ 13 dB/s (type I) ou 9 dB/s (type II). Une valeur de 13,3 dB/s est donc le choix « norme », 40 à 60 dB/s le choix « type REAPER » ; le défaut retenu, 24 dB/s, se situe entre les deux (choix de l'utilisateur).

  Un profil RMS personnalisé = changer `pMeterRmsMs` (réglage non automatisable) ; la fenêtre se met à jour sans reconstruction d'IR.

---

## 6. A/B

Le DSP n'a aucune notion d'A/B : ce sont uniquement des valeurs de paramètres, à gérer côté UI. Ce qu'il faut savoir pour décider :
- **a)** Les paramètres d'IR peuvent faire partie du jeu A/B. Changer de slot déclenche **une seule** reconstruction (le débounce de 60 ms regroupe les 10 valeurs envoyées d'un coup) et l'audio ne coupe pas (fondu de 21 ms). Coût : voir le tableau de la section 3 (de l'ordre de la milliseconde à quelques centaines de ms selon le rééchantillonnage).
- **b)** Le chemin est un state (pas un paramètre) : le mettre dans l'A/B revient à faire `setState("ir_path", chemin)` au changement de slot ; il sera regroupé avec les paramètres dans la même reconstruction. Contrainte : il relit le WAV s'il diffère du précédent (0,1 à 3 ms mesurés).
C'est un choix de conception pour toi : techniquement, tout est faisable.

---

## 7. Divers

- **Entrées / sorties audio** : 1 port stéréo en entrée, 1 port stéréo en sortie. **Pas** d'entrée sidechain, **pas** de MIDI. Vérifié par le test d'hôte (« 1 in / 1 out, 2 canaux »).
- **Presets internes** : **aucun** pour l'instant (aucun « programme » DPF déclaré). Les valeurs des paramètres et le chemin de l'IR sont sauvegardés avec le projet. Format de presets à décider (programmes DPF ou fichiers gérés par l'UI).
- **Échelles proposées pour les potards** (le DSP accepte n'importe quelle courbe ; seules les fréquences et les Q sont marqués logarithmiques) :
  - `pPreDelay` 0–2000 ms : échelle **non linéaire** conseillée (puissance ≈ 2,5 ou logarithmique avec 0 au minimum), sinon les petites valeurs sont inaccessibles.
  - `pDry` / `pWet` : linéaire **en dB**, avec le cran minimum affiché « -inf » (≤ −100).
  - `pInGain`, `pOutGain` : linéaire en dB. `pWidth` : linéaire en %.
  - Fréquences d'EQ et Q : logarithmique.
- **Paramètres automatisables** : 28 (gains, dry/wet, pré-délai, largeur, EQ, passe-haut, passe-bas). Les autres (IR, fenêtre RMS, sorties) ne le sont pas.

---

## Annexe : liste complète des paramètres (59)

| Indice | Nom de code | Nom |
|---|---|---|
| 0–5 | `pInGain, pOutGain, pDry, pWet, pPreDelay, pWidth` | niveaux, mixage, pré-délai, largeur |
| 6 | `pEqOn` | interrupteur général de l'EQ |
| 7–11 / 12–16 / 17–21 | `pB1On…pB1Q` / `pB2On…pB2Q` / `pB3On…pB3Q` | bandes 1, 2, 3 : On, Type, Freq, Gain, Q |
| 22–24 | `pHpOn, pHpFreq, pHpSlope` | passe-haut |
| 25–27 | `pLpOn, pLpFreq, pLpSlope` | passe-bas |
| 28–37 | `pInputMode, pStretch, pAutoTrim, pBegin, pEnd, pAutoLen, pAttack, pDecay, pReverse, pNorm` | retouches d'IR (non automatisables) |
| 38–39 | `pIRLengthMs, pIRStatus` | sorties : longueur et statut de l'IR |
| 40 | `pMeterRmsMs` | fenêtre RMS des vu-mètres (10–3000 ms, défaut 300) |
| 41–44 | `pMInPeakL, pMInPeakR, pMOutPeakL, pMOutPeakR` | sorties : crête (dBFS) |
| 45–48 | `pMInRmsL, pMInRmsR, pMOutRmsL, pMOutRmsR` | sorties : RMS (dB) |
| 49–52 | `pMInLufsM, pMOutLufsM, pMInLufsS, pMOutLufsS` | sorties : LUFS-M, LUFS-S |
| 53 | `pMeterPeakDecay` | retombée de la crête, 5–300 dB/s, défaut 24 (non automatisable) |
| 54 | `pMeterReset` | déclencheur : 1 = remise à zéro des crêtes max (se réarme seul) |
| 55–58 | `pMInPeakMaxL, pMInPeakMaxR, pMOutPeakMaxL, pMOutPeakMaxR` | sorties : crête maximale depuis la remise à zéro (dBFS) |

Les plages, valeurs par défaut et libellés sont dans `plugin/LowLatConvParams.h`.
