# Compiler pour Linux, Windows et macOS avec GitHub Actions

Tu n'as pas besoin de Mac : GitHub fournit des machines macOS, Windows et Linux. Le fichier
`.github/workflows/build.yml` lance, à chaque envoi de fichiers (« push ») :

| Tâche | Où | Ce qu'elle fait |
|---|---|---|
| `tests-linux` | Ubuntu | compile et exécute toute la suite de tests du DSP |
| `tests-macos` | macOS (Apple silicon) | idem : valide aussi le code ARM64 / NEON |
| `tests-windows` | Windows (MSYS2) | idem ; *peut échouer au début* (n'empêche pas le reste) |
| `plugin` (linux-x86_64) | Ubuntu | compile `LowLatConv.clap` pour Linux |
| `plugin` (win64) | Ubuntu, compilation croisée MinGW | compile `LowLatConv.clap` pour Windows |
| `plugin` (macos-universal) | macOS | compile `LowLatConv.clap` pour Intel **et** Apple silicon |

## Le principe en une phrase
Le **dépôt** (« repository ») est un dossier hébergé sur github.com qui garde l'historique de tes fichiers. Dès que
tu y envoies des fichiers, GitHub exécute le workflow et te donne les plugins compilés à télécharger.

## Chemin A : tout par le site web (sans installer Git)
1. Crée un compte sur https://github.com (active la double authentification dans les réglages).
2. Clique **New repository**. Nom : `LowLatConv` (ou autre). Choisis **Public** (les minutes de calcul sont
   gratuites ; en privé il y a un quota mensuel et les minutes macOS comptent plus). Ne coche pas « Add a README ».
3. Décompresse `lowlat_conv.tar.gz`. **Le dossier `lowlat_conv` est la racine du dépôt** : c'est son *contenu*
   (`Makefile`, `plugin/`, `.github/`...) qui doit se retrouver à la racine du dépôt, pas le dossier lui-même.
   Attention : `.github` et `.gitignore` commencent par un point, donc sont **cachés** par défaut
   (Windows : Explorateur > Affichage > Éléments masqués ; macOS : Cmd+Maj+. dans le Finder ; Linux : Ctrl+H).
4. Sur la page du dépôt vide, clique **uploading an existing file**, fais glisser **tout le contenu** du dossier
   (y compris `.github`), écris un message (« Premier envoi »), puis **Commit changes**.
   Si le glisser-déposer saute le dossier caché `.github`, crée-le à la main : **Add file > Create new file**, tape
   `.github/workflows/build.yml` comme nom, colle le contenu du fichier et valide.
5. Ouvre l'onglet **Actions** : le workflow « build » démarre tout seul (comptez de 5 à 20 minutes).
   Ce chemin ne demande aucune commande ; DPF est récupéré automatiquement par le workflow.

## Chemin B : avec Git en ligne de commande (recommandé à terme)
Installe Git (https://git-scm.com ; sous Windows, il fournit « Git Bash »), puis dans le dossier du projet :
```
git config --global user.name  "Ton Nom"
git config --global user.email "toi@exemple.fr"
git init
git add .
git commit -m "LowLatConv"
git branch -M main
git remote add origin https://github.com/<ton-compte>/LowLatConv.git
git push -u origin main
```
Au premier `push`, GitHub demande de se connecter (une fenêtre de navigateur s'ouvre sous Windows/macOS ; sinon
installe GitHub CLI et lance `gh auth login`).
Pour figer la version de DPF au lieu de la récupérer à chaque fois : `git submodule add https://github.com/DISTRHO/DPF dpf`,
puis supprime dans `build.yml` l'étape qui récupère `DISTRHO/DPF` (et utilise `submodules: recursive` dans la première).

## Récupérer les plugins compilés
Onglet **Actions** > clique sur le dernier run > section **Artifacts** en bas : `LowLatConv-linux-x86_64`,
`LowLatConv-win64`, `LowLatConv-macos-universal` (zip contenant `LowLatConv.clap`).

## Quand ça échoue (ce sera probablement le cas au début)
Une croix rouge = une étape a échoué. Ouvre la tâche en rouge, déplie l'étape en rouge, copie les ~50 dernières
lignes et envoie-les-moi dans la conversation. Pour tout le journal : roue dentée en haut à droite du run >
**Download log archive**. Je n'ai pas accès à GitHub : je ne vois que ce que tu me colles ou me déposes.
Après correction, **Re-run jobs** relance sans rien renvoyer, ou envoie les fichiers corrigés (ci-dessous).

## Mettre à jour le dépôt quand je te donne de nouveaux fichiers
- Chemin A : sur la page du dépôt, **Add file > Upload files**, glisse les fichiers nouveaux ou modifiés
  (même nom = remplacés), **Commit changes**. Pour supprimer un fichier : ouvre-le > icône corbeille.
- Chemin B : copie les fichiers dans le dossier du projet (en remplaçant), puis
  `git add -A`, `git commit -m "description"`, `git push`.
Chaque envoi relance automatiquement les tests et les compilations.

## Bonnes habitudes
- Un message de commit court qui dit ce qui change (« EQ : correction de la pente 48 dB »).
- Ne pas envoyer les dossiers `build/` et `bin/` (le fichier `.gitignore` s'en charge).
- Pour publier une version : onglet **Releases** > **Create a new release** > ajoute un numéro (`v0.1.0`) et
  glisse-y les zips des plugins téléchargés depuis les Artifacts.
- **Licence** : un projet open source doit contenir un fichier `LICENSE` choisi par toi (GitHub propose des modèles :
  Add file > Create new file > nom `LICENSE` > « Choose a license template »). PFFFT (type BSD), dr_wav (domaine
  public / MIT-0) et DPF (ISC) sont compatibles avec la plupart des choix ; la police Inter (OFL) exige d'inclure
  son fichier de licence à côté de la police. Indique ensuite la licence dans `getLicense()` (plugin/LowLatConvPlugin.cpp).

## À savoir
- **macOS** : le plugin n'est pas signé ni notarisé (cela demande un compte Apple Developer payant). Après
  téléchargement, macOS le bloque ; pour tester : `xattr -dr com.apple.quarantine LowLatConv.clap`.
- **Windows** : pas de signature non plus ; SmartScreen peut avertir.
- Je n'ai **pas pu exécuter ces workflows** : l'action `distrho/dpf-makefile-action@v1` est celle qu'utilisent
  les exemples de DPF et d'autres projets DPF (cibles `linux-x86_64`, `win64`, `macos-universal`), mais je n'ai pas
  pu lire sa documentation ici. Le fichier de workflow est une base à ajuster d'après les premiers journaux.
- Quand l'interface existera : passer `DISTRHO_PLUGIN_HAS_UI` à 1 dans `plugin/DistrhoPluginInfo.h` et ajouter
  `LLC_WITH_UI: true` dans `env:` du job `plugin` pour compiler DGL.
