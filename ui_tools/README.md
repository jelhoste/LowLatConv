# Outils de vérification de l'interface (sans OpenGL)

`nvg_soft.hpp` : un moteur de rendu **logiciel** pour NanoVG. Le vrai cœur de NanoVG (`nanovg.c`, fontstash,
stb_truetype, pris dans DPF) fait la géométrie et la mise en page du texte ; ce fichier remplace seulement le
moteur OpenGL par un rasteriseur CPU (dégradés linéaires / radiaux / « box », découpe, mélange alpha,
suréchantillonnage 3x pour le lissage). Il sert à **voir ce que dessine le code de l'interface sur ma machine**
(sans fenêtre ni GPU) et à le comparer aux captures de la maquette HTML.

Limites : ce n'est pas le rendu OpenGL exact (pas de frange d'anti-crénelage de NanoVG, découpe à bord franc,
traits très fins un peu différents). Il valide la géométrie, les couleurs, les dégradés, le texte et la découpe ;
l'aspect final au pixel près reste à juger sur ton écran.

    # NanoVG de DPF est du C++ (il est compilé comme tel dans DPF)
    g++ -O2 -w -x c++ -std=c++17 -c dpf/dgl/src/nanovg/nanovg.c -Idpf/dgl/src/nanovg -o nanovg.o
    g++ -std=c++17 -O2 -Idpf/dgl/src/nanovg ui_tools/demo.cpp nanovg.o -o nvg_demo
    ./nvg_demo /chemin/vers/police.ttf     # écrit /tmp/nvg_soft_demo.png
