# NAM TEST — sources, state BUILD67

A fork of Mike Oliphant's [`neural-amp-modeler-lv2`](https://github.com/mikeoliphant/neural-amp-modeler-lv2)
(GPL-3.0), for the MOD Dwarf.

Only the files that were **changed or added** live here. The rest of the plugin
(NeuralAudio, Eigen, the LV2 headers) comes from the original repository.
`modifications.diff` holds the complete diff, **added files included**: on a
fresh clone of the original repository, `git apply modifications.diff` gives
back exactly the tree delivered here.

---

## Installing the binary

`dist/nam_test_build67.tar.gz` holds the bundle ready to drop in, built for
aarch64 (`-mcpu=cortex-a35`), libstdc++ linked statically, symbols stripped.
It is the same file as the release attachment.

The bundle is called **`nam_test.lv2`** — not `neural_amp_modeler.lv2`, which
is the name the store's NAM uses. The build produces it under that name
directly, so there is nothing left to rename by hand.

```bash
tar xzf nam_test_build67.tar.gz

COPYFILE_DISABLE=1 tar czf - --exclude='._*' nam_test.lv2 \
  | ssh root@192.168.51.1 'rm -rf /root/.lv2/nam_test.lv2 && tar xzf - -C /root/.lv2'

ssh root@192.168.51.1 'systemctl restart mod-ui'
```

Settings for the ports that went away are ignored on reload: an existing
pedalboard reopens, but a footswitch assignment that pointed at one of them is
lost. If the panel keeps its old look, force a page reload — the browser holds
the old template in cache.

---

## Rebuilding

```bash
git clone --recurse-submodules https://github.com/mikeoliphant/neural-amp-modeler-lv2 nam
cd nam
# overwrite with the files from this repository, then:
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=../tools/aarch64.cmake \
         -DCMAKE_CXX_FLAGS="-mcpu=cortex-a35" -DCMAKE_C_FLAGS="-mcpu=cortex-a35"
make -j4
```

The toolchain file links **libstdc++ statically**: the machine caps at
GLIBCXX_3.4.26 (GCC 9), and without that the plugin does not load.

### Regenerating what is generated

```bash
python3 tools/gen_names.py      # src/fav_names.h, the table of 125 names
python3 tools/gen_ports.py      # /tmp/ports.ttl, descriptor ports 7 to 84
python3 tools/gen_settings.py   # resources/modgui/settings.html
python3 tools/verif_ports.py    # do the descriptor and the C struct agree?
```

The tables feed **both** the C code and the descriptor, so they cannot drift
apart. That came out of a crash — see below. `verif_ports.py` reads the two and
compares them port by port; it must report nothing before a delivery.

### Checking before installing

```bash
aarch64-linux-gnu-gcc -O1 -o loadtest tools/loadtest.c -I deps/lv2/include -ldl
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./loadtest build/src/neural_amp_modeler.so

aarch64-linux-gnu-gcc -O1 -o statetest tools/statetest.c -I deps/lv2/include -I src -ldl
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./statetest build/src/neural_amp_modeler.so
```

The first bench instantiates the plugin with all 85 ports connected, runs it,
works every control and frees it. The second saves the state, creates a fresh
instance, hands that state back and checks that **the picked names come back
into the ports**. Both are to be run before every delivery.

---

## What the plugin does on top of the original

**Navigation** — scans `/data/user-files/NAM Models` two levels deep;
`Model` (rank), `Next`, `Prev`, `Browse`/`Load`, `Rescan`. The audio thread
never reads the list: it sends a rank to the worker, which resolves and loads.

**Ten favorites** — one file, one gain, one name picked from a list, one load
button and one store button each. `Fav Next` steps through the non-empty ones,
`Cycle` bounds the round.

**Auto-gain** — loads each favorite off the audio thread, makes it process
deterministic noise, measures the energy, takes the **median** as the reference,
and **writes the corrections into the knobs** through the kx extension. `Undo`
puts the previous values back.

**CV input** — 0 to 10 V cut into ten bands, one per favorite.

**The machine's screen** — footswitch label and full-screen popup when the
favorite changes, through the HMI extension.

**A favorite's name** — a rank in the table of 125 seven-character names, one
enumerated control port per favorite (`Name 1` to `Name 10`). The rank is kept
in the plugin's state under a key of its own; on reload the plugin **asks the
host for it back** through the kx extension, so the panel's list and the
machine's screen both start again on the right name, even when the host does
not repost the port value itself.

Free typing was **removed**. On the Starless image a string parameter does not
come back down to the plugin: the text had to be sent up one character at a
time through a control port, and returned the same way. Eleven ports and a
timer in the browser, for a name — the list does the same work without any of
it.

**Trace** — `/tmp/nam_test_state.txt`, rewritten every two seconds: real
version, addressed ports and their capabilities, kx state, auto-gain state and
corrections, the name picked for each favorite. The plugin log does not come
out on this machine; this is the only reliable diagnostic channel.

---

## The traps that cost the most

1. **A URI table written by hand** with 8 entries when `NUM_FAVS` had moved to
   10: two `nullptr` handed to the host, SEGV on instantiation, **jackd dies**.
   Hence the generators.
2. **Everything was exported** — 9943 symbols. mod-host loads every plugin into
   a single process: two forks of the same code get mixed up there. Hence
   `export.map`, which lets nothing out but `lv2_descriptor`.
3. **A CV port is read at the first sample**, never the last: unconnected, the
   host may allocate a single value.
4. `stat()` only exists from GLIBC_2.33 on → use `d_type` from `dirent`.
5. glibc's `log`, `exp` and `log10` are 2.29 → an approximate `log2` is written
   inside the plugin.
6. **The *Momentary* flag and the screen capabilities arrive empty**: `info` is
   non-null but entirely zero. Treat zero as "everything allowed", and
   recognise a foot press by how long the state stays high.
7. **A string parameter does not come back down to the plugin** on the Starless
   image. The whole free-typing machinery — text field, character encoded into
   a port, return channel — existed only to work around that. An enumerated
   control port always gets through: that is what carries the name now.
8. **Two ports written in the same cycle reach the browser in any order.** The
   measured correction therefore landed in the previous favorite's cell. The
   favorite's number now goes out a tenth of a second AFTER the value it points
   at, and it is the number that triggers the display.
9. **Add nothing to the original icon**: the bottom strip is taken by the model
   selector, and anything put there overlaps it.
10. **Diagnostic witnesses lie too.** The trace's "build" line was frozen text,
    and the kx state conflated "absent" with "never asked". Two false trails
    followed for nothing.

---

## What changed in BUILD67

* **Everything is in English**: panel labels, the names in the list, port help
  texts, the trace file, the code and its comments, the tools and this README.
  The list keeps its 125 entries at the same ranks, so a saved pedalboard still
  points at the same name.
* `outils/` became `tools/`, `etattest.c` became `statetest.c`, and
  `LISEZ-MOI.md` became `README.md`.

## What changed in BUILD66

* **Free typing is gone**: no more text field in the panel. A favorite's name
  is picked from the drop-down list, and **the choice is saved** — in the
  plugin's state, then put back into the control port on reload.
* Eleven ports left with it: `web_slot`, `web_char`, `web_strobe`, `name_slot`
  and `n1..n7`. The descriptor holds **85** of them instead of 95.
* `name_slot` became `db_slot`: it now names nothing but the favorite whose
  correction `auto_db_slot` carries, and it arrives a tenth of a second after
  that correction so the two always belong together.
* Picking the NAM model did not move: file selector, `Model`, `Next`, `Prev`,
  `Browse`/`Load`, `Rescan`, favorites, all unchanged.
* The build produces the bundle under its real name, **`nam_test.lv2`**: no
  renaming by hand afterwards, and no confusing it with the store's
  `neural_amp_modeler.lv2`.
* `tools/verif_ports.py` compares the descriptor and the C struct port by port;
  `tools/statetest.c` checks that the names survive a reload.
* `gen_settings.py` and `gen_ports.py` generate *exactly* the delivered files
  again: they had fallen behind them.

---

Licence: GPL-3.0, like the original plugin.
