# NAM TEST — sources, état BUILD65

Fork de `neural-amp-modeler-lv2` de Mike Oliphant (GPL-3.0), pour MOD Dwarf.

Seuls les fichiers **modifiés ou ajoutés** sont ici. Le reste du plugin
(NeuralAudio, Eigen, les en-têtes LV2) vient du dépôt d'origine.
`modifications.diff` contient le diff complet, si tu préfères repartir d'un
clone frais.

---

## Reconstruire

```bash
git clone --recurse-submodules https://github.com/mikeoliphant/neural-amp-modeler-lv2 nam
cd nam
# écraser avec les fichiers de cette archive, puis :
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=../outils/aarch64.cmake \
         -DCMAKE_CXX_FLAGS="-mcpu=cortex-a35" -DCMAKE_C_FLAGS="-mcpu=cortex-a35"
make -j4
```

Le fichier de chaîne lie **libstdc++ en statique** : la machine plafonne à
GLIBCXX_3.4.26 (GCC 9), et sans ça le plugin ne se charge pas.

### Régénérer ce qui est engendré

```bash
python3 outils/gen_names.py      # src/fav_names.h, la table des 125 noms
python3 outils/gen_ports.py      # la table des ports du descripteur
python3 outils/gen_settings.py   # resources/modgui/settings.html
```

Les tables servent **à la fois** au code C et au descripteur : elles ne peuvent
donc pas diverger. C'est né d'un plantage — voir plus bas.

### Contrôler avant d'installer

```bash
aarch64-linux-gnu-gcc -O1 -o loadtest outils/loadtest.c -I deps/lv2/include -ldl
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./loadtest build/src/neural_amp_modeler.so
```

Le banc instancie le plugin avec ses 95 ports, le fait tourner, actionne chaque
commande et le libère. À passer avant chaque livraison.

---

## Ce que le plugin fait, en plus de l'original

**Navigation** — scan de `/data/user-files/NAM Models` sur deux niveaux ;
`Model` (rang), `Next`, `Prev`, `Browse`/`Load`, `Rescan`. Le thread audio ne
lit jamais la liste : il envoie un rang au worker, qui résout et charge.

**Dix favoris** — un fichier, un gain, un nom en liste, un nom libre, un bouton
charger et un bouton ranger chacun. `Fav Next` parcourt les non vides, `Cycle`
limite le tour.

**Auto-gain** — charge chaque favori hors du thread audio, lui fait traiter un
bruit déterministe, mesure l'énergie, prend la **médiane** comme référence, et
**écrit les corrections dans les boutons** par l'extension kx. `Annuler` remet
les valeurs d'avant.

**Entrée CV** — 0 à 10 V découpés en dix bandes, une par favori.

**Écran de la machine** — libellé du footswitch et popup plein écran au
changement de favori, par l'extension HMI.

**Saisie de texte depuis le navigateur** — un port porte à la fois le compteur,
l'emplacement et le caractère ; le nom **remonte** par `name_slot` + `n1..n7`,
un favori par seconde.

**Trace** — `/tmp/nam_test_state.txt`, réécrite toutes les deux secondes :
version réelle, ports assignés et leurs capacités, état kx, état de l'auto-gain
et corrections, noms. Le journal du plugin ne ressort pas sur cette machine ;
c'est le seul canal de diagnostic fiable.

---

## Les pièges qui ont coûté cher

1. **Table d'URI écrite à la main** avec 8 entrées quand `NUM_FAVS` est passé à
   10 : deux `nullptr` passés à l'hôte, SEGV à l'instanciation, **jackd meurt**.
   D'où les générateurs.
2. **Tout était exporté** — 9943 symboles. mod-host charge tous les plugins dans
   un seul processus : deux forks du même code s'y mélangent. D'où `export.map`,
   qui ne laisse sortir que `lv2_descriptor`.
3. **Un port CV se lit au premier échantillon**, jamais au dernier : non
   connecté, l'hôte peut n'allouer qu'une valeur.
4. `stat()` n'existe qu'à partir de GLIBC_2.33 → `d_type` de `dirent`.
5. `log`, `exp`, `log10` de la glibc sont en 2.29 → un `log2` approché est
   écrit dans le plugin.
6. **Drapeau *Momentary* et capacités d'écran arrivent vides** : `info` non nul
   mais entièrement à zéro. Traiter zéro comme « tout permis », et reconnaître
   l'appui du pied à la durée de l'état haut.
7. Dans la pedalboard, **seuls les widgets de mod-ui reçoivent les clics** :
   mod-ui recopie l'icône et les liaisons du script disparaissent. Un
   `<input type="text">` y échappe, puisqu'il ne demande aucune liaison.
8. **La fonction d'envoi doit être refaite à chaque `start`** : capturée une
   fois, elle écrit à travers les `funcs` d'une interface que mod-ui a déjà
   remplacée, et rien n'arrive.
9. **Ne rien ajouter à l'icône d'origine** : la bande du bas est occupée par le
   sélecteur de modèle, tout ce qu'on y pose se superpose.
10. **Les témoins de diagnostic mentent aussi.** La ligne « build » de la trace
    était un texte figé, et l'état kx confondait « absent » et « jamais
    demandé ». Deux fausses pistes suivies pour rien.

---

Licence : GPL-3.0, comme le plugin d'origine.
