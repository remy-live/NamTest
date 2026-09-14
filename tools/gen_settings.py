# -*- coding: utf-8 -*-
"""Generates resources/modgui/settings.html, the mod-ui settings panel.

Each of the ten name lists carries all 125 entries of FAV_NAME_TABLE: writing
that by hand makes no sense, and the panel has to offer EXACTLY the same names
as the plugin. The table therefore comes from src/fav_names.h, as it does for
the descriptor.

    python3 outils/gen_settings.py
"""
import io
import re

NAMES = re.findall(r'\t"([^"]*)",', open('src/fav_names.h').read())
N = 10

# mod-ui stacks the cells in document order; the grid is flex, so "order" is
# what really decides. One favorite takes a group of ten: 110 for the file
# picker (set by the stylesheet), 111 the name, and the rest after it.
FAV_ORDER = lambda i, k: 100 + i * 10 + k


def adr(sym):
    return ('<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<span class="mod-knob-snapshot-status icon-camera" '
            'mod-role="input-control-snapshotable" mod-port-symbol="%s"></span>') % (sym, sym)


def switch(sym, title, order):
    return ('    <div class="mod-switch x-cell" style="order:%d"><span class="mod-switch-title">%s</span>'
            '<div class="mod-switch-background"></div>'
            '<div class="mod-switch-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (order, title, sym, adr(sym))


def knob(sym, title, order):
    return ('    <div class="mod-knob x-cell" style="order:%d"><span class="mod-knob-title">%s</span>'
            '<span class="mod-knob-min-value" mod-role="input-control-minimum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-max-value" mod-role="input-control-maximum" mod-port-symbol="%s"></span>'
            '<span class="mod-knob-current-value" mod-role="input-control-value" mod-port-symbol="%s"></span>'
            '<div class="mod-knob-background"></div>'
            '<div class="mod-knob-image" mod-role="input-control-port" mod-port-symbol="%s" mod-widget="film"></div>'
            '%s</div>\n') % (order, title, sym, sym, sym, sym, adr(sym))


def enum(sym, title, options, order):
    opts = ''.join('<div mod-role="enumeration-option" mod-port-value="%d">%s</div>' % (v, o)
                   for v, o in options)
    return ('    <div class="mod-enumerated x-cell" style="order:%d"><span class="mod-enumerated-title">%s</span>'
            '<div class="mod-address {{#isPerformanceView}}mod-hidden{{/isPerformanceView}}" '
            'mod-role="input-control-address" mod-port-symbol="%s"></div>'
            '<div class="mod-enumerated-list" mod-role="input-control-port" mod-port-symbol="%s" '
            'mod-widget="custom-select">%s</div>'
            '<span class="mod-knob-snapshot-status icon-camera" mod-role="input-control-snapshotable" '
            'mod-port-symbol="%s"></span></div>\n') % (order, title, sym, sym, opts, sym)


def measured(i, order):
    """The correction the auto-gain found for this favorite.

    Filled in by script-nam.js from the db_slot and auto_db_slot outputs: a
    template cannot read an output port on its own.
    """
    return ('    <div class="x-cell nam-measured" style="order:%d"><span class="nam-measured-title">Auto %d</span>'
            '<span class="nam-auto" data-slot="%d">+0.00 dB</span></div>\n') % (order, i, i)


def brk(order):
    return '    <div class="x-break" style="order:%d"></div>\n' % order


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

# The file pickers: mod-ui builds them itself from the descriptor's parameters.
# So we loop over them, and the stylesheet gives each one its place (#model
# first, then one favorite per group of ten).
s.write('''            {{#effect.parameters}}
            {{#path}}
            <div class="mod-enumerated x-cell x-file" mod-role="input-parameter" mod-parameter-uri="{{uri}}" mod-widget="custom-select-path">
                <span class="mod-enumerated-title">{{label}}</span>
                <div mod-role="input-parameter-value" mod-parameter-uri="{{uri}}" class="mod-enumerated-selected">-- choose a model --</div>
                <div class="mod-enumerated-list">
                    {{#files}}
                    <div mod-role="enumeration-option" mod-parameter-value="{{fullname}}">{{basename}}</div>
                    {{/files}}
                </div>
            </div>
            {{/path}}
            {{/effect.parameters}}
''')

# --- current sound and navigation
s.write(brk(11))
s.write(knob('input_level', 'Input', 12))
s.write(knob('output_level', 'Output', 13))
s.write(enum('quality_scale', 'Quality', [(0, 'LITE'), (1, 'FULL')], 14))
s.write(knob('model_index', 'Rank', 15))
s.write(brk(16))
for k, (sym, title) in enumerate([('step_prev', 'Previous'), ('step_next', 'Next'),
                                  ('browse', 'Browse'), ('action', 'Load'),
                                  ('rescan', 'Rescan folder')]):
    s.write(switch(sym, title, 17 + k))

# --- favorites: shared controls
s.write(brk(25))
s.write('    <div class="x-line-title" style="order:26">FAVORITES</div>\n')
s.write(brk(27))
s.write(switch('fav_browse', 'Next favorite', 30))
s.write(knob('cycle_count', 'Cycle length', 31))
s.write(switch('auto_gain', 'Auto gain', 32))
s.write(knob('fav_gain', 'Gain outside favorites', 33))
s.write(knob('store_slot', 'Slot', 34))
s.write(switch('store', 'Store into slot', 35))
s.write(switch('auto_apply', 'Apply gains', 37))
s.write(switch('auto_undo', 'Undo', 38))
s.write(brk(39))

# --- one favorite per group of ten: name, gain, load, store
options = list(enumerate(NAMES))
for i in range(1, N + 1):
    s.write(enum('fav_name_%d' % i, 'Name %d' % i, options, FAV_ORDER(i, 1)))
    s.write(knob('fav_gain_%d' % i, 'Gain %d' % i, FAV_ORDER(i, 2)))
    s.write(switch('fav_%d' % i, 'Load %d' % i, FAV_ORDER(i, 3)))
    s.write(switch('fav_store_%d' % i, 'Store here %d' % i, FAV_ORDER(i, 4)))
    s.write(brk(FAV_ORDER(i, 5)))

# The measured correction slips in next to the name: same "order", so document
# order settles it and the cell lands right after the list.
for i in range(1, N + 1):
    s.write(measured(i, FAV_ORDER(i, 1)))

s.write('        </div>\n    </div>\n</div>\n')

open('resources/modgui/settings.html', 'w').write(s.getvalue())
print("template written: %d bytes, %d names per list, %d favorites"
      % (len(s.getvalue()), len(NAMES), N))
