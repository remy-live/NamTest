# -*- coding: utf-8 -*-
"""Engendre resources/modgui/settings.html, le panneau de reglages de mod-ui.

Les dix listes de noms comptent chacune les 125 entrees de FAV_NAME_TABLE :
ecrire cela a la main n'a aucun sens, et le panneau doit proposer EXACTEMENT
les memes noms que le plugin. La table vient donc de src/fav_names.h, comme
pour le descripteur.

    python3 outils/gen_settings.py
"""
import io
import re

NOMS = re.findall(r'\t"([^"]*)",', open('src/fav_names.h').read())
N = 10

# mod-ui empile les cellules dans l'ordre du document ; la grille est en flex,
# donc c'est « order » qui decide vraiment. Un favori occupe une dizaine :
# 110 pour le fichier (pose par la feuille de style), 111 le nom, et la suite.
ORDRE_FAV = lambda i, k: 100 + i * 10 + k


def adr(sym):
    return ('<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<span class="mod-knob-snapshot-status icon-camera" '
            'mod-role="input-control-snapshotable" mod-port-symbol="%s"></span>') % (sym, sym)


def switch(sym, titre, ordre):
    return ('    <div class="mod-switch x-cell" style="order:%d"><span class="mod-switch-title">%s</span>'
            '<div class="mod-switch-background"></div>'
            '<div class="mod-switch-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (ordre, titre, sym, adr(sym))


def knob(sym, titre, ordre):
    return ('    <div class="mod-knob x-cell" style="order:%d"><span class="mod-knob-title">%s</span>'
            '<span class="mod-knob-min-value" mod-role="input-control-minimum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-max-value" mod-role="input-control-maximum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-current-value" mod-role="input-control-value" mod-port-symbol="%s"></span>'
            '<div class="mod-knob-background"></div>'
            '<div class="mod-knob-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (ordre, titre, sym, sym, sym, sym, adr(sym))


def enum(sym, titre, options, ordre):
    opts = ''.join('<div mod-role="enumeration-option" mod-port-value="%d">%s</div>' % (v, o)
                   for v, o in options)
    return ('    <div class="mod-enumerated x-cell" style="order:%d"><span class="mod-enumerated-title">%s</span>'
            '<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<div class="mod-enumerated-list" mod-role="input-control-port" mod-port-symbol="%s" '
            'mod-widget="custom-select">%s</div>'
            '<span class="mod-knob-snapshot-status icon-camera" mod-role="input-control-snapshotable" '
            'mod-port-symbol="%s"></span></div>\n') % (ordre, titre, sym, sym, opts, sym)


def mesure(i, ordre):
    """La correction trouvee par l'auto-gain pour ce favori.

    Remplie par script-nam.js depuis les sorties db_slot et auto_db_slot : un
    port de sortie ne se lit pas tout seul dans un gabarit.
    """
    return ('    <div class="x-cell nam-mesure" style="order:%d"><span class="nam-mesure-title">Auto %d</span>'
            '<span class="nam-auto" data-slot="%d">+0.00 dB</span></div>\n') % (ordre, i, i)


def brk(ordre):
    return '    <div class="x-break" style="order:%d"></div>\n' % ordre


s = io.StringIO()
s.write('''<div class="mod-pedal-settings x-set{{{cns}}}">
    <header class="clearfix">
        <h1 class="alignleft">{{effect.name}}
            <span class="plugin-label">{{#userLabel}} - {{userLabel}}{{/userLabel}}</span>
            <span class="mod-edit"></span>
            <span class="plugin-global-snapshot icon-camera"></span>
        </h1>
        <button class="js-close alignright btn btn-danger">Close</button>
    </header>
    <div class="mod-controls clearfix">
        <div class="mod-control-group clearfix x-sec-grid">
            <div class="mod-switch bypass x-cell" style="order:1">
                <span class="mod-switch-title">ON/OFF</span>
                <div class="mod-light on" mod-role="bypass-light"></div>
                <div class="mod-switch-background"></div>
                <div class="mod-switch-image" mod-role="bypass"></div>
                <div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" mod-role="bypass-address"></div>
                <span class="mod-knob-snapshot-status icon-camera" mod-role="bypass-snapshotable"></span>
            </div>
''')

# Les selecteurs de fichier : mod-ui les engendre lui-meme a partir des
# parametres du descripteur. On boucle donc sur eux, et la feuille de style
# donne a chacun sa place (#model en tete, puis un favori par dizaine).
s.write('''            {{#effect.parameters}}
            {{#path}}
            <div class="mod-enumerated x-cell x-file" mod-role="input-parameter" mod-parameter-uri="{{uri}}" mod-widget="custom-select-path">
                <span class="mod-enumerated-title">{{label}}</span>
                <div mod-role="input-parameter-value" mod-parameter-uri="{{uri}}" class="mod-enumerated-selected">-- choisir un modele --</div>
                <div class="mod-enumerated-list">
                    {{#files}}
                    <div mod-role="enumeration-option" mod-parameter-value="{{fullname}}">{{basename}}</div>
                    {{/files}}
                </div>
            </div>
            {{/path}}
            {{/effect.parameters}}
''')

# --- son courant et navigation
s.write(brk(11))
s.write(knob('input_level', 'Entree', 12))
s.write(knob('output_level', 'Sortie', 13))
s.write(enum('quality_scale', 'Qualite', [(0, 'LITE'), (1, 'FULL')], 14))
s.write(knob('model_index', 'Rang', 15))
s.write(brk(16))
for k, (sym, titre) in enumerate([('step_prev', 'Precedent'), ('step_next', 'Suivant'),
                                  ('browse', 'Parcourir'), ('action', 'Charger'),
                                  ('rescan', 'Relire le dossier')]):
    s.write(switch(sym, titre, 17 + k))

# --- favoris : commandes communes
s.write(brk(25))
s.write('    <div class="x-line-title" style="order:26">FAVORIS</div>\n')
s.write(brk(27))
s.write(switch('fav_browse', 'Favori suivant', 30))
s.write(knob('cycle_count', 'Longueur du cycle', 31))
s.write(switch('auto_gain', 'Auto gain', 32))
s.write(knob('fav_gain', 'Gain hors favori', 33))
s.write(knob('store_slot', 'Emplacement', 34))
s.write(switch('store', "Ranger dans l'emplacement", 35))
s.write(switch('auto_apply', 'Appliquer les gains', 37))
s.write(switch('auto_undo', 'Annuler', 38))
s.write(brk(39))

# --- un favori par dizaine : nom, gain, charger, ranger
options = list(enumerate(NOMS))
for i in range(1, N + 1):
    s.write(enum('fav_name_%d' % i, 'Nom %d' % i, options, ORDRE_FAV(i, 1)))
    s.write(knob('fav_gain_%d' % i, 'Gain %d' % i, ORDRE_FAV(i, 2)))
    s.write(switch('fav_%d' % i, 'Charger %d' % i, ORDRE_FAV(i, 3)))
    s.write(switch('fav_store_%d' % i, 'Ranger ici %d' % i, ORDRE_FAV(i, 4)))
    s.write(brk(ORDRE_FAV(i, 5)))

# La correction mesuree se glisse a cote du nom : meme « order », donc l'ordre
# du document departage, et la cellule vient juste apres la liste.
for i in range(1, N + 1):
    s.write(mesure(i, ORDRE_FAV(i, 1)))

s.write('        </div>\n    </div>\n</div>\n')

open('resources/modgui/settings.html', 'w').write(s.getvalue())
print("gabarit ecrit : %d octets, %d noms par liste, %d favoris"
      % (len(s.getvalue()), len(NOMS), N))
