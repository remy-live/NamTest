# NAM TEST — sources, état BUILD66

Fork de `neural-amp-modeler-lv2` de Mike Oliphant (GPL-3.0), pour MOD Dwarf.

Seuls les fichiers **modifiés ou ajoutés** sont ici. Le reste du plugin
(NeuralAudio, Eigen, les en-têtes LV2) vient du dépôt d'origine.
`modifications.diff` contient le diff complet, fichiers **ajoutés compris** :
sur un clone frais du dépôt d'origine, `git apply modifications.diff` redonne
exactement l'arbre livré ici.

---

## Installer le binaire livré

`dist/nam-test-build66-aarch64.tar.gz` contient le bundle prêt à poser, compilé
pour aarch64 (`-mcpu=cortex-a35`), libstdc++ en statique, symboles retirés.
C'est le même fichier que la pièce jointe de la release.

```bash
tar xzf dist/nam-test-build66-aarch64.tar.gz
scp -r neural_amp_modeler.lv2 root@192.168.51.1:/root/.lv2/
# puis redémarrer la pédale, pour que mod-ui relise les greffons
```

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
python3 outils/gen_ports.py      # /tmp/ports.ttl, les ports 7 à 84 du descripteur
python3 outils/gen_settings.py   # resources/modgui/settings.html
python3 outils/verif_ports.py    # le descripteur et la structure C sont-ils d'accord
```

Les tables servent **à la fois** au code C et au descripteur : elles ne peuvent
donc pas diverger. C'est né d'un plantage — voir plus bas. `verif_ports.py`
relit les deux et compare port par port ; il ne doit rien signaler avant une
livraison.

### Contrôler avant d'installer

```bash
aarch64-linux-gnu-gcc -O1 -o loadtest outils/loadtest.c -I deps/lv2/include -ldl
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./loadtest build/src/neural_amp_modeler.so

aarch64-linux-gnu-gcc -O1 -o etattest outils/etattest.c -I deps/lv2/include -I src -ldl
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./etattest build/src/neural_amp_modeler.so
```

Le premier banc instancie le plugin avec ses 85 ports, le fait tourner, actionne
chaque commande et le libère. Le second sauvegarde l'état, crée une instance
neuve, lui rend cet état et vérifie que **les noms choisis reviennent dans les
ports**. Les deux sont à passer avant chaque livraison.

---

## Ce que le plugin fait, en plus de l'original

**Navigation** — scan de `/data/user-files/NAM Models` sur deux niveaux ;
`Model` (rang), `Next`, `Prev`, `Browse`/`Load`, `Rescan`. Le thread audio ne
lit jamais la liste : il envoie un rang au worker, qui résout et charge.

**Dix favoris** — un fichier, un gain, un nom choisi dans une liste, un bouton
charger et un bouton ranger chacun. `Fav Next` parcourt les non vides, `Cycle`
limite le tour.

**Auto-gain** — charge chaque favori hors du thread audio, lui fait traiter un
bruit déterministe, mesure l'énergie, prend la **médiane** comme référence, et
**écrit les corrections dans les boutons** par l'extension kx. `Annuler` remet
les valeurs d'avant.

**Entrée CV** — 0 à 10 V découpés en dix bandes, une par favori.

**Écran de la machine** — libellé du footswitch et popup plein écran au
changement de favori, par l'extension HMI.

**Le nom d'un favori** — un rang dans la table des 125 noms, un port de contrôle
énuméré par favori (`Name 1` à `Name 10`). Le rang est rangé dans l'état du
plugin, sous une clé à lui ; à la reprise, le plugin le **redemande à l'hôte**
par l'extension kx, si bien que la liste du panneau et l'écran de la machine
repartent tous deux sur le bon nom, même quand l'hôte ne repose pas lui-même la
valeur du port.

La saisie libre a été **retirée**. Sur l'image Starless un paramètre de type
chaîne ne redescend pas jusqu'au plugin : il fallait remonter le texte caractère
par caractère à travers un port de contrôle, et le renvoyer de même. Onze ports
et un minuteur dans le navigateur pour un nom — la liste fait le même travail
sans rien de tout cela.

**Trace** — `/tmp/nam_test_state.txt`, réécrite toutes les deux secondes :
version réelle, ports assignés et leurs capacités, état kx, état de l'auto-gain
et corrections, nom choisi pour chaque favori. Le journal du plugin ne ressort pas sur cette machine ;
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
7. **Un paramètre de type chaîne ne redescend pas jusqu'au plugin** sur l'image
   Starless. Toute la saisie libre — champ texte, encodage du caractère dans un
   port, canal de retour — n'existait que pour contourner cela. Un port de
   contrôle énuméré passe toujours : c'est lui qui porte le nom désormais.
8. **Deux ports écrits dans le même cycle arrivent au navigateur dans un ordre
   quelconque.** La correction mesurée se posait donc dans la case du favori
   précédent. Le numéro du favori part maintenant un dixième de seconde APRÈS
   la valeur qu'il désigne, et c'est lui qui déclenche l'affichage.
9. **Ne rien ajouter à l'icône d'origine** : la bande du bas est occupée par le
   sélecteur de modèle, tout ce qu'on y pose se superpose.
10. **Les témoins de diagnostic mentent aussi.** La ligne « build » de la trace
    était un texte figé, et l'état kx confondait « absent » et « jamais
    demandé ». Deux fausses pistes suivies pour rien.

---

## Ce qui a changé en BUILD66

* Le **nom libre** disparaît : plus de champ texte dans le panneau. Le nom d'un
  favori se choisit dans la liste déroulante, et **le choix est enregistré** —
  dans l'état du plugin, puis reposé dans le port de contrôle à la reprise.
* Onze ports partent avec lui : `web_slot`, `web_char`, `web_strobe`,
  `name_slot` et `n1..n7`. Le descripteur en compte **85** au lieu de 95.
* `name_slot` devient `db_slot` : il ne désigne plus que le favori dont
  `auto_db_slot` porte la correction mesurée, et il arrive un dixième de seconde
  après elle pour que les deux aillent toujours ensemble.
* Le choix du modèle NAM ne bouge pas : sélecteur de fichier, `Model`, `Next`,
  `Prev`, `Browse`/`Load`, `Rescan`, favoris, tout reste tel quel.
* `outils/verif_ports.py` compare le descripteur et la structure C port par
  port ; `outils/etattest.c` vérifie que les noms survivent au rechargement.
* `gen_settings.py` et `gen_ports.py` engendrent de nouveau *exactement* les
  fichiers livrés : ils avaient pris du retard sur eux.

---

Licence : GPL-3.0, comme le plugin d'origine.
