# Table unique des noms de favoris -- sept caracteres au maximum, largeur d'un
# libelle de footswitch. Le code C et le descripteur sortent d'ici.
BASES_NUM = [
    ("CLEAN", 4), ("CRUNCH", 3), ("DRIVE", 4), ("LEAD", 3), ("SOLO", 3),
    ("RYTHME", 3), ("BOOST", 2), ("FUZZ", 2), ("METAL", 2), ("BLUES", 2),
    ("ROCK", 2), ("FUNK", 2), ("JAZZ", 2), ("SON", 8), ("AMPLI", 4),
]

SIMPLES = [
    "INTRO", "FINAL", "COUPLET", "REFRAIN", "PONT", "BACKING", "ARPEGE",
    "PUNK", "REGGAE", "COUNTRY", "SURF", "GRUNGE", "STONER", "DJENT",
    "SHRED", "GOSPEL",
    "TWEED", "PLEXI", "JCM800", "AC30", "DELUXE", "TWIN", "BASSMAN",
    "PRINCE", "RECTO", "MARK", "JC120", "SUPRO", "HIWATT", "ORANGE",
    "SOLDANO", "FRIEDMN", "DUMBLE", "ZWRECK", "BOGNER", "DIEZEL", "ENGL",
    "MATCHLS", "BADCAT", "SILVER", "BLACKFC", "BROWN",
    "TS808", "TIMMY", "KLON", "BIGMUFF", "OCTAVE", "TREBLE",
    "GRIT", "EDGE", "CHIME", "GLASS", "VELOURS", "GRAS", "FIN", "CREUX",
    "MEDIUM", "CHAUD", "CLAIR", "SOMBRE", "PUNCH", "MORDANT", "DOUX",
]

NOMS = ["AUTO"]

for base, n in BASES_NUM:
    NOMS.append(base)
    for k in range(1, n + 1):
        # espace si la place le permet, colle sinon : sept caracteres maximum
        candidat = "%s %d" % (base, k)
        if len(candidat) > 7:
            candidat = "%s%d" % (base, k)
        NOMS.append(candidat)

NOMS += SIMPLES

trop_long = [n for n in NOMS if len(n) > 7]
assert not trop_long, "noms trop longs : %s" % trop_long
assert len(NOMS) == len(set(NOMS)), "doublons : %s" % [n for n in NOMS if NOMS.count(n) > 1]

with open('src/fav_names.h', 'w') as f:
    f.write("/* ENGENDRE par gen_names.py -- ne pas modifier a la main.\n"
            " * Table unique des noms de favoris, partagee par le code et le\n"
            " * descripteur : seule facon qu'ils ne divergent jamais.\n"
            " * Sept caracteres au maximum, largeur d'un libelle de footswitch. */\n"
            "#pragma once\n\n"
            "static const char* const FAV_NAME_TABLE[] = {\n")
    for n in NOMS:
        f.write('\t"%s",\n' % n)
    f.write("};\n\n#define FAV_NAME_COUNT %d\n" % len(NOMS))

bloc = ""
for i in range(1, 9):
    bloc += """	], [
		a lv2:ControlPort, lv2:InputPort;
		lv2:index %d;
		lv2:symbol "fav_name_%d";
		lv2:name "Name %d";
		lv2:default 0;
		lv2:minimum 0;
		lv2:maximum %d;
		lv2:portProperty lv2:enumeration, lv2:integer;
		rdfs:comment "Nom affiche pour le favori %d - AUTO reprend le nom du fichier";
""" % (49 + i, i, i, len(NOMS) - 1, i)
    for k, n in enumerate(NOMS):
        bloc += '\t\tlv2:scalePoint [ rdfs:label "%s"; rdf:value %d ];\n' % (n, k)

open('/tmp/fav_name_ports.ttl', 'w').write(bloc)
print("%d noms" % len(NOMS))
print("exemples :", ", ".join(NOMS[1:14]))
