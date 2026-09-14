import re
N = 10   # favoris

P = []   # (symbole, nom, type, extra)
def ctl(sym, nom, **kw): P.append(('ctl', sym, nom, kw))
def out(sym, nom, **kw): P.append(('out', sym, nom, kw))
def cv(sym, nom, **kw):  P.append(('cv', sym, nom, kw))

ctl('model_index','Model', mini=0, maxi=199, defaut=0, entier=True,
    aide="Rang du modele dans le dossier NAM Models")
for s,n,a in [('step_next','Next',"Modele suivant"),('step_prev','Prev',"Modele precedent"),
              ('rescan','Rescan',"Relire le dossier des modeles")]:
    ctl(s,n, toggle=True, aide=a)
out('model_count','Count', mini=0, maxi=200, entier=True)
out('current_index','Current', mini=-1, maxi=199, defaut=-1, entier=True)
out('load_status','Status', mini=-1, maxi=2, entier=True,
    aide="-1 echec, 0 rien, 1 en cours, 2 charge")
ctl('browse','Browse', toggle=True, aide="Parcourir SANS charger")
ctl('action','Load', toggle=True, aide="Charger le modele parcouru")
out('browse_index','Browsing', mini=0, maxi=199, entier=True)
out('active_fav','Active Fav', mini=0, maxi=N, entier=True)
for i in range(1, N+1):
    ctl('fav_%d'%i,'Fav %d'%i, toggle=True, grp=i, aide="Charger le favori %d"%i)
ctl('store_slot','Slot', mini=1, maxi=N, defaut=1, entier=True, aide="Emplacement vise par Store")
ctl('store','Store', toggle=True, aide="Ranger le modele courant dans l'emplacement Slot")
ctl('fav_gain','Gain', mini=-20.0, maxi=20.0, defaut=0.0, db=True,
    aide="Gain hors favori")
out('applied_gain','Gain Applied', mini=-20.0, maxi=20.0, defaut=0.0, db=True)
out('store_count','Stores', mini=0, maxi=999, entier=True)
ctl('fav_browse','Fav Next', toggle=True, aide="Passer au favori non vide suivant")
for i in range(1, N+1):
    ctl('fav_gain_%d'%i,'Gain %d'%i, mini=-20.0, maxi=20.0, defaut=0.0, db=True, grp=i,
        aide="Gain du favori %d"%i)
for i in range(1, N+1):
    out('fav_index_%d'%i,'Idx %d'%i, mini=-1, maxi=199, defaut=-1, entier=True, grp=i)
ctl('cycle_count','Cycle', mini=1, maxi=N, defaut=N, entier=True,
    aide="Nombre de favoris parcourus par Fav Next")
out('hmi_state','HMI', mini=0, maxi=999, entier=True,
    aide="unites = etat, dizaines = capacites de l'ecran")
NOMS = open('src/fav_names.h').read()
NOMS = re.findall(r'\t"([^"]*)",', NOMS)
for i in range(1, N+1):
    ctl('fav_name_%d'%i,'Name %d'%i, mini=0, maxi=len(NOMS)-1, defaut=0, enum=NOMS, grp=i,
        aide="Nom affiche pour le favori %d"%i)
for i in range(1, N+1):
    ctl('fav_store_%d'%i,'Store %d'%i, toggle=True, grp=i,
        aide="Ranger le modele qui joue dans le favori %d"%i)
ctl('auto_gain','Auto Gain', toggle=True, aide="Mesurer et egaliser le volume de tous les favoris")
out('auto_state','Auto', mini=-1, maxi=2, entier=True, aide="-1 echec, 0 rien, 1 en cours, 2 fait")
out('auto_offset','Auto dB', mini=-12.0, maxi=12.0, defaut=0.0, db=True,
    aide="Correction mesuree du favori actif")
cv('cv_select','CV Fav', mini=0.0, maxi=10.0, defaut=0.0,
    aide="Choisit le favori par tension : la plage 0-10 V est decoupee en %d bandes egales" % N)
out('db_slot','Fav dB Slot', mini=0, maxi=N, entier=True,
    aide="Favori dont Auto dB Slot porte la correction - tourne une fois par seconde")
out('auto_db_slot','Auto dB Slot', mini=-12.0, maxi=12.0, defaut=0.0, db=True,
    aide="Correction mesuree du favori designe par Fav dB Slot")
ctl('auto_apply','Appliquer', toggle=True,
    aide="Poser les corrections mesurees dans les boutons de gain")
ctl('auto_undo','Annuler', toggle=True,
    aide="Remettre les gains d'avant l'application")
out('kx_state','KX', mini=-1, maxi=2, defaut=0, entier=True,
    aide="0 l'hote ne permet pas d'ecrire dans les boutons, 1 possible, 2 accepte, -1 refuse")

# --- descripteur
sortie = []
for k, (genre, sym, nom, kw) in enumerate(P):
    idx = 7 + k
    t = {'ctl': 'lv2:ControlPort, lv2:InputPort',
         'out': 'lv2:ControlPort, lv2:OutputPort',
         'cv':  'lv2:InputPort, lv2:CVPort, mod:CVPort'}[genre]
    b = '\t], [\n\t\ta %s;\n\t\tlv2:index %d;\n\t\tlv2:symbol "%s";\n\t\tlv2:name "%s";\n' % (t, idx, sym, nom)
    if kw.get('toggle'):
        b += '\t\tlv2:default 0;\n\t\tlv2:minimum 0;\n\t\tlv2:maximum 1;\n'
        b += '\t\tlv2:portProperty lv2:toggled, mod:preferMomentaryOnByDefault;\n'
    else:
        b += '\t\tlv2:default %s;\n\t\tlv2:minimum %s;\n\t\tlv2:maximum %s;\n' % (
            kw.get('defaut', 0), kw.get('mini', 0), kw.get('maxi', 1))
        if kw.get('entier'):
            b += '\t\tlv2:portProperty lv2:integer;\n'
        if kw.get('enum'):
            b += '\t\tlv2:portProperty lv2:enumeration, lv2:integer;\n'
        if kw.get('db'):
            b += '\t\tunits:unit units:db;\n'
    if kw.get('aide'):
        b += '\t\trdfs:comment "%s";\n' % kw['aide']
    # l'entree CV n'appartient a aucun groupe : mod-ui la dessine sur le boitier
    if genre != 'cv':
        b += '\t\tpg:group <@NAM_LV2_ID@#groupe%d>;\n' % kw.get('grp', 0)
    for v, n in enumerate(kw.get('enum', [])):
        b += '\t\tlv2:scalePoint [ rdfs:label "%s"; rdf:value %d ];\n' % (n, v)
    sortie.append(b)

open('/tmp/ports.ttl','w').write(''.join(sortie) + '\t].')

# --- ordre attendu, pour le controle et pour la structure C
open('/tmp/ordre.txt','w').write('\n'.join([p[1] for p in P]))
print("%d ports engendres, du 7 au %d, total %d avec les 7 d'origine"
      % (len(P), 7 + len(P) - 1, 7 + len(P)))
