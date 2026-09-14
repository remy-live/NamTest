"""Checks that the descriptor and the C struct talk about the SAME ports.

connect_port() indexes struct Ports by port number: add a port on one side only
and the host writes into the wrong field. The project's first crash came from
exactly that. Run this after any change to the ports, before delivering.

    python3 outils/verif_ports.py
"""
import re
import sys

TTL = 'resources/neural_amp_modeler.ttl.in'
HEADER = 'src/nam_plugin.h'

# The C field and the descriptor symbol do not always carry the same name.
# An array of NUM_FAVS pointers is NUM_FAVS ports, numbered from 1.
ARRAYS = {
    'fav': 'fav_%d',
    'favGain': 'fav_gain_%d',
    'favIndex': 'fav_index_%d',
    'favNamePort': 'fav_name_%d',
    'favStore': 'fav_store_%d',
}

ALIAS = {
    'audio_in': 'input',
    'audio_out': 'output',
}


def descriptor_ports():
    s = open(TTL).read()
    pairs = re.findall(r'lv2:index (\d+);\s*\n\s*lv2:symbol "([^"]+)";', s)
    return [(int(i), sym) for i, sym in pairs]


def struct_fields():
    s = open(HEADER).read()

    nfavs = int(re.search(r'NUM_FAVS = (\d+);', s).group(1))

    begin = s.index('struct Ports {')
    end = s.index('};', begin)
    body = s[begin:end]

    fields = []
    for line in body.split('\n'):
        m = re.match(r'\s*(?:const\s+)?[\w:_]+\*?\s*\*?\s*(\w+)(\[[^\]]+\])?;', line.strip())
        if not m or 'struct Ports' in line:
            continue
        name, array = m.group(1), m.group(2)
        if array is None:
            fields.append(ALIAS.get(name, name))
        elif name in ARRAYS:
            fields += [ARRAYS[name] % (k + 1) for k in range(nfavs)]
        else:
            size = int(array.strip('[]'))
            fields += ['%s[%d]' % (name, k) for k in range(size)]
    return fields


def main():
    faults = []

    ports = descriptor_ports()
    expected = list(range(len(ports)))

    if [i for i, _ in ports] != expected:
        faults.append('the descriptor indices do not run in sequence: %s'
                      % [i for i, _ in ports])

    total = int(re.search(r'NUM_PORTS_TOTAL = (\d+);', open(HEADER).read()).group(1))

    if total != len(ports):
        faults.append('NUM_PORTS_TOTAL says %d, the descriptor holds %d ports'
                      % (total, len(ports)))

    fields = struct_fields()

    if len(fields) != len(ports):
        faults.append('struct Ports holds %d slots, the descriptor %d'
                      % (len(fields), len(ports)))

    for (i, sym), field in zip(ports, fields):
        if sym != field:
            faults.append('port %d: descriptor "%s", struct "%s"' % (i, sym, field))

    for f in faults:
        print('FAULT:', f)

    if faults:
        return 1

    print('%d ports, descriptor and C struct agree' % len(ports))
    return 0


if __name__ == '__main__':
    sys.exit(main())
