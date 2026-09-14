Fork de [neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2)
de Mike Oliphant (GPL-3.0), pour MOD Dwarf.

## Ce qui change

* La **saisie libre disparaît** du panneau : plus de champ texte. Le nom d'un
  favori se choisit dans la liste déroulante des 125 noms.
* **Le choix est enregistré** : le rang est rangé dans l'état du plugin, sous
  une clé à lui, et reposé dans le port de contrôle à la reprise par
  l'extension kx — même quand l'hôte ne repose pas lui-même la valeur. La liste
  du panneau et l'écran de la machine repartent tous deux sur le bon nom.
* **Le choix du modèle NAM ne bouge pas** : sélecteur de fichier, `Model`,
  `Next`, `Prev`, `Browse`/`Load`, `Rescan`, favoris, tout reste tel quel.
* Onze ports partent avec la saisie (`web_slot`, `web_char`, `web_strobe`,
  `name_slot`, `n1..n7`) : le descripteur en compte **85** au lieu de 95.
* `name_slot` devient `db_slot` : il ne désigne plus que le favori dont
  `auto_db_slot` porte la correction mesurée, et il est écrit un dixième de
  seconde **après** elle — deux ports changés dans le même cycle arrivent au
  navigateur dans un ordre que rien ne garantit, et la correction se posait
  alors dans la case du favori précédent.

Pourquoi la saisie libre ne pouvait pas rester : sur l'image Starless, un
paramètre de type chaîne ne redescend pas jusqu'au plugin. Le texte devait
monter caractère par caractère à travers un port de contrôle et redescendre par
sept autres. Un port énuméré passe toujours, lui.

## Installer

```bash
tar xzf nam-test-build66-aarch64.tar.gz
scp -r neural_amp_modeler.lv2 root@192.168.51.1:/root/.lv2/
```

Puis redémarrer la pédale, pour que mod-ui relise les greffons.

Compilé pour aarch64 (`-mcpu=cortex-a35`), libstdc++ en statique, symboles
retirés, rien au-delà de `GLIBC_2.27`.

## Contrôles passés, sous qemu-aarch64

| banc | résultat |
|---|---|
| `loadtest` | 85 ports branchés, chaque commande actionnée, pas de plantage |
| `etattest` | les dix noms survivent au rechargement |
| `verif_ports` | 85 ports, descripteur et structure C d'accord |
| `nm -D` | un seul symbole exporté, `lv2_descriptor` |
| `objdump -T` | rien au-delà de `GLIBC_2.27` |

Licence : GPL-3.0, comme le plugin d'origine.
