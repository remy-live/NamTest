A fork of [neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2)
by Mike Oliphant (GPL-3.0), for the MOD Dwarf.

## What changed

**Everything is in English now.** The plugin was written in French throughout;
it is being shared, so the whole surface has been translated:

* the settings panel — labels, buttons, section titles, the file picker;
* **the 125 favorite names**, which show on the pedal's footswitch display:
  `RYTHME` became `RHYTHM`, `SON` and `AMPLI` became `TONE` and `AMP`,
  `COUPLET` and `REFRAIN` became `VERSE` and `CHORUS`, `VELOURS` / `CHAUD` /
  `SOMBRE` became `VELVET` / `WARM` / `DARK`, and so on — **at the same ranks**,
  so a saved pedalboard still points at the same entry;
* the descriptor's help texts, which mod-ui shows as tooltips, and the port
  names `Appliquer` and `Annuler`, now `Apply` and `Undo`;
* `/tmp/nam_test_state.txt`, the trace file used for diagnosis;
* the code: comments, function and variable names, the test benches and the
  generators;
* the documentation: `LISEZ-MOI.md` became `README.md`, `outils/` became
  `tools/`, and `etattest.c` became `statetest.c`.

Nothing about the plugin's behaviour changed with it. The audio code, the port
list and the saved-state format are untouched.

Two small fixes made along the way: the trace printed a hard-coded port number
`31` where the real one is 33, and the "no favorite" label copied one byte too
many.

## Installing

The bundle is called **`nam_test.lv2`**, as before: it replaces the one already
on the machine, and does not get mixed up with the store's NAM.

```bash
tar xzf nam_test_build67.tar.gz

COPYFILE_DISABLE=1 tar czf - --exclude='._*' nam_test.lv2 \
  | ssh root@192.168.51.1 'rm -rf /root/.lv2/nam_test.lv2 && tar xzf - -C /root/.lv2'

ssh root@192.168.51.1 'systemctl restart mod-ui'
```

If the panel keeps its old look after the restart, force a page reload — the
browser holds the previous template in cache.

Built for aarch64 (`-mcpu=cortex-a35`), libstdc++ linked statically, symbols
stripped, nothing beyond `GLIBC_2.27`.

## Checks passed, under qemu-aarch64

| bench | result |
|---|---|
| `loadtest` | 85 ports connected, every control worked, no crash |
| `statetest` | the ten names survive a reload |
| `verif_ports` | 85 ports, descriptor and C struct agree |
| `nm -D` | one exported symbol, `lv2_descriptor` |
| `objdump -T` | nothing beyond `GLIBC_2.27` |

Licence: GPL-3.0, like the original plugin.
