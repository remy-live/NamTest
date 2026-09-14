"""Controle que le descripteur et la structure C parlent des MEMES ports.

connect_port() indexe la structure Ports par le numero de port : un port ajoute
d'un cote seulement, et l'hote ecrit dans le mauvais champ. Le premier plantage
du projet vient exactement de la. Ce controle est a passer apres toute
modification des ports, avant de livrer.

    python3 outils/verif_ports.py
"""
import re
import sys

TTL = 'resources/neural_amp_modeler.ttl.in'
ENTETE = 'src/nam_plugin.h'

# Le champ C et le symbole du descripteur ne portent pas toujours le meme nom.
# Un tableau de NUM_FAVS pointeurs vaut NUM_FAVS ports, numerotes a partir de 1.
TABLEAUX = {
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


def ports_du_descripteur():
    s = open(TTL).read()
    paires = re.findall(r'lv2:index (\d+);\s*\n\s*lv2:symbol "([^"]+)";', s)
    return [(int(i), sym) for i, sym in paires]


def champs_de_la_structure():
    s = open(ENTETE).read()

    nfavs = int(re.search(r'NUM_FAVS = (\d+);', s).group(1))

    debut = s.index('struct Ports {')
    fin = s.index('};', debut)
    corps = s[debut:fin]

    champs = []
    for ligne in corps.split('\n'):
        m = re.match(r'\s*(?:const\s+)?[\w:_]+\*?\s*\*?\s*(\w+)(\[[^\]]+\])?;', ligne.strip())
        if not m or 'struct Ports' in ligne:
            continue
        nom, tab = m.group(1), m.group(2)
        if tab is None:
            champs.append(ALIAS.get(nom, nom))
        elif nom in TABLEAUX:
            champs += [TABLEAUX[nom] % (k + 1) for k in range(nfavs)]
        else:
            taille = int(tab.strip('[]'))
            champs += ['%s[%d]' % (nom, k) for k in range(taille)]
    return champs


def main():
    fautes = []

    ports = ports_du_descripteur()
    attendus = list(range(len(ports)))

    if [i for i, _ in ports] != attendus:
        fautes.append('les index du descripteur ne se suivent pas : %s'
                      % [i for i, _ in ports])

    total = int(re.search(r'NUM_PORTS_TOTAL = (\d+);', open(ENTETE).read()).group(1))

    if total != len(ports):
        fautes.append('NUM_PORTS_TOTAL vaut %d, le descripteur compte %d ports'
                      % (total, len(ports)))

    champs = champs_de_la_structure()

    if len(champs) != len(ports):
        fautes.append('la structure Ports compte %d emplacements, le descripteur %d'
                      % (len(champs), len(ports)))

    for (i, sym), champ in zip(ports, champs):
        if sym != champ:
            fautes.append('port %d : descripteur "%s", structure "%s"' % (i, sym, champ))

    for f in fautes:
        print('FAUTE :', f)

    if fautes:
        return 1

    print('%d ports, descripteur et structure C d\'accord' % len(ports))
    return 0


if __name__ == '__main__':
    sys.exit(main())
