import re, io

NOMS = re.findall(r'\t"([^"]*)",', open('src/fav_names.h').read())
N = 10
URI = "http://remy.local/nam-test"

def adr(sym):
    return ('<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<span class="mod-knob-snapshot-status icon-camera" '
            'mod-role="input-control-snapshotable" mod-port-symbol="%s"></span>') % (sym, sym)

def switch(sym, titre):
    return ('    <div class="mod-switch x-cell"><span class="mod-switch-title">%s</span>'
            '<div class="mod-switch-background"></div>'
            '<div class="mod-switch-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (titre, sym, adr(sym))

def knob(sym, titre):
    return ('    <div class="mod-knob x-cell"><span class="mod-knob-title">%s</span>'
            '<span class="mod-knob-min-value" mod-role="input-control-minimum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-max-value" mod-role="input-control-maximum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-current-value" mod-role="input-control-value" mod-port-symbol="%s"></span>'
            '<div class="mod-knob-background"></div>'
            '<div class="mod-knob-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (titre, sym, sym, sym, sym, adr(sym))

def enum(sym, titre, options):
    opts = ''.join('<div mod-role="enumeration-option" mod-port-value="%d">%s</div>' % (v, o)
                   for v, o in options)
    return ('    <div class="mod-enumerated x-cell"><span class="mod-enumerated-title">%s</span>'
            '<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<div class="mod-enumerated-list" mod-role="input-control-port" mod-port-symbol="%s" '
            'mod-widget="custom-select">%s</div>'
            '<span class="mod-knob-snapshot-status icon-camera" mod-role="input-control-snapshotable" '
            'mod-port-symbol="%s"></span></div>\n') % (titre, sym, sym, opts, sym)

def fichier(uri_frag, titre):
    u = "%s#%s" % (URI, uri_frag)
    return ('    <div class="mod-enumerated x-cell x-file" mod-role="input-parameter" '
            'mod-parameter-uri="%s" mod-widget="custom-select-path">'
            '<span class="mod-enumerated-title">%s</span>'
            '<div mod-role="input-parameter-value" mod-parameter-uri="%s" class="mod-enumerated-selected">'
            '-- choisir un modele --</div>'
            '<div class="mod-enumerated-list"></div></div>\n') % (u, titre, u)

brk = '    <div class="x-break"></div>\n'

s = io.StringIO()
s.write('<div class="mod-pedal-settings x-set{{{cns}}}">\n')
s.write('''    <header class="clearfix">
        <h1 class="alignleft">{{effect.name}}
            <span class="plugin-label">{{#userLabel}} - {{userLabel}}{{/userLabel}}</span>
            <span class="mod-edit"></span>
            <span class="plugin-global-snapshot icon-camera"></span>
        </h1>
        <button class="js-close alignright btn btn-danger">Close</button>
    </header>
    <div class="mod-controls clearfix">
        <div class="mod-control-group clearfix">
            <div class="mod-switch bypass">
                <span class="mod-switch-title">ON/OFF</span>
                <div class="mod-light on" mod-role="bypass-light"></div>
                <div class="mod-switch-background"></div>
                <div class="mod-switch-image" mod-role="bypass"></div>
                <div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" mod-role="bypass-address"></div>
                <span class="mod-knob-snapshot-status icon-camera" mod-role="bypass-snapshotable"></span>
            </div>
''')

# --- section navigation
s.write('<div class="x-sec"><div class="x-sec-title">Son courant et navigation</div><div class="x-sec-grid">\n')
s.write(fichier('model', 'Modele courant'))
s.write(brk)
s.write(knob('input_level', 'Entree'))
s.write(knob('output_level', 'Sortie'))
s.write(enum('quality_scale', 'Qualite', [(0, 'LITE'), (1, 'FULL')]))
s.write(knob('model_index', 'Rang'))
s.write(brk)
for sym, t in [('step_prev', 'Precedent'), ('step_next', 'Suivant'), ('browse', 'Parcourir'),
               ('action', 'Charger'), ('rescan', 'Relire le dossier')]:
    s.write(switch(sym, t))
s.write('</div></div>\n')

# --- section favoris : reglages communs
s.write('<div class="x-sec"><div class="x-sec-title">Favoris - commandes communes</div><div class="x-sec-grid">\n')
s.write(switch('fav_browse', 'Favori suivant'))
s.write(knob('cycle_count', 'Longueur du cycle'))
s.write(switch('auto_gain', 'Auto gain'))
s.write(knob('fav_gain', 'Gain hors favori'))
s.write(brk)
s.write(knob('store_slot', 'Emplacement'))
s.write(switch('store', 'Ranger dans l\'emplacement'))
s.write('</div></div>\n')

# --- une section par favori : fichier, nom, gain, charger, ranger
options = list(enumerate(NOMS))
for i in range(1, N + 1):
    s.write('<div class="x-sec"><div class="x-sec-title">Favori %d</div><div class="x-sec-grid">\n' % i)
    s.write(fichier('fav%d' % i, 'Modele'))
    s.write(enum('fav_name_%d' % i, 'Nom', options))
    s.write(knob('fav_gain_%d' % i, 'Gain'))
    s.write(switch('fav_%d' % i, 'Charger'))
    s.write(switch('fav_store_%d' % i, 'Ranger ici'))
    s.write(brk)
    s.write('</div></div>\n')

s.write('        </div>\n    </div>\n</div>\n')
open('resources/modgui/settings.html', 'w').write(s.getvalue())
print("gabarit ecrit : %d octets, %d sections" % (len(s.getvalue()), N + 2))
