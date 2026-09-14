"""Generates the descriptor's port block, indices 7 to 84.

    python3 outils/gen_ports.py

The result lands in /tmp/ports.ttl and replaces everything from index 7 to the
end of resources/neural_amp_modeler.ttl.in. The C struct Ports must follow the
same order -- outils/verif_ports.py checks that it does.
"""
import re

N = 10   # favorites

P = []   # (kind, symbol, name, extra)
def ctl(sym, name, **kw): P.append(('ctl', sym, name, kw))
def out(sym, name, **kw): P.append(('out', sym, name, kw))
def cv(sym, name, **kw):  P.append(('cv', sym, name, kw))

ctl('model_index','Model', lo=0, hi=199, default=0, integer=True,
    help="Rank of the model inside the NAM Models folder")
for s,n,a in [('step_next','Next',"Next model"),('step_prev','Prev',"Previous model"),
              ('rescan','Rescan',"Re-read the model folder")]:
    ctl(s,n, toggle=True, help=a)
out('model_count','Count', lo=0, hi=200, integer=True)
out('current_index','Current', lo=-1, hi=199, default=-1, integer=True)
out('load_status','Status', lo=-1, hi=2, integer=True,
    help="-1 failed, 0 idle, 1 loading, 2 loaded")
ctl('browse','Browse', toggle=True, help="Browse WITHOUT loading")
ctl('action','Load', toggle=True, help="Load the browsed model")
out('browse_index','Browsing', lo=0, hi=199, integer=True)
out('active_fav','Active Fav', lo=0, hi=N, integer=True)
for i in range(1, N+1):
    ctl('fav_%d'%i,'Fav %d'%i, toggle=True, group=i, help="Load favorite %d"%i)
ctl('store_slot','Slot', lo=1, hi=N, default=1, integer=True,
    help="Slot that Store writes into")
ctl('store','Store', toggle=True, help="Store the current model into the slot named by Slot")
ctl('fav_gain','Gain', lo=-20.0, hi=20.0, default=0.0, db=True,
    help="Gain outside any favorite")
out('applied_gain','Gain Applied', lo=-20.0, hi=20.0, default=0.0, db=True)
out('store_count','Stores', lo=0, hi=999, integer=True)
ctl('fav_browse','Fav Next', toggle=True, help="Step to the next non-empty favorite")
for i in range(1, N+1):
    ctl('fav_gain_%d'%i,'Gain %d'%i, lo=-20.0, hi=20.0, default=0.0, db=True, group=i,
        help="Gain of favorite %d"%i)
for i in range(1, N+1):
    out('fav_index_%d'%i,'Idx %d'%i, lo=-1, hi=199, default=-1, integer=True, group=i)
ctl('cycle_count','Cycle', lo=1, hi=N, default=N, integer=True,
    help="How many favorites Fav Next steps through")
out('hmi_state','HMI', lo=0, hi=999, integer=True,
    help="units = state, tens = the screen's capabilities")
NAMES = open('src/fav_names.h').read()
NAMES = re.findall(r'\t"([^"]*)",', NAMES)
for i in range(1, N+1):
    ctl('fav_name_%d'%i,'Name %d'%i, lo=0, hi=len(NAMES)-1, default=0, enum=NAMES, group=i,
        help="Name displayed for favorite %d"%i)
for i in range(1, N+1):
    ctl('fav_store_%d'%i,'Store %d'%i, toggle=True, group=i,
        help="Store the playing model into favorite %d"%i)
ctl('auto_gain','Auto Gain', toggle=True,
    help="Measure and even out the volume of every favorite")
out('auto_state','Auto', lo=-1, hi=2, integer=True, help="-1 failed, 0 idle, 1 running, 2 done")
out('auto_offset','Auto dB', lo=-12.0, hi=12.0, default=0.0, db=True,
    help="Measured correction for the active favorite")
cv('cv_select','CV Fav', lo=0.0, hi=10.0, default=0.0,
    help="Picks the favorite by voltage: the 0-10 V range is cut into %d equal bands" % N)
out('db_slot','Fav dB Slot', lo=0, hi=N, integer=True,
    help="Favorite whose correction Auto dB Slot carries - rotates once per second")
out('auto_db_slot','Auto dB Slot', lo=-12.0, hi=12.0, default=0.0, db=True,
    help="Measured correction for the favorite named by Fav dB Slot")
ctl('auto_apply','Apply', toggle=True,
    help="Put the measured corrections into the gain knobs")
ctl('auto_undo','Undo', toggle=True,
    help="Put back the gains from before the apply")
out('kx_state','KX', lo=-1, hi=2, default=0, integer=True,
    help="0 the host does not allow writing the knobs, 1 possible, 2 accepted, -1 refused")

# --- descriptor
result = []
for k, (kind, sym, name, kw) in enumerate(P):
    idx = 7 + k
    t = {'ctl': 'lv2:ControlPort, lv2:InputPort',
         'out': 'lv2:ControlPort, lv2:OutputPort',
         'cv':  'lv2:InputPort, lv2:CVPort, mod:CVPort'}[kind]
    b = '\t], [\n\t\ta %s;\n\t\tlv2:index %d;\n\t\tlv2:symbol "%s";\n\t\tlv2:name "%s";\n' % (t, idx, sym, name)
    if kw.get('toggle'):
        b += '\t\tlv2:default 0;\n\t\tlv2:minimum 0;\n\t\tlv2:maximum 1;\n'
        b += '\t\tlv2:portProperty lv2:toggled, mod:preferMomentaryOnByDefault;\n'
    else:
        b += '\t\tlv2:default %s;\n\t\tlv2:minimum %s;\n\t\tlv2:maximum %s;\n' % (
            kw.get('default', 0), kw.get('lo', 0), kw.get('hi', 1))
        if kw.get('integer'):
            b += '\t\tlv2:portProperty lv2:integer;\n'
        if kw.get('enum'):
            b += '\t\tlv2:portProperty lv2:enumeration, lv2:integer;\n'
        if kw.get('db'):
            b += '\t\tunits:unit units:db;\n'
    if kw.get('help'):
        b += '\t\trdfs:comment "%s";\n' % kw['help']
    # the CV input belongs to no group: mod-ui draws it on the pedal itself
    if kind != 'cv':
        b += '\t\tpg:group <@NAM_LV2_ID@#groupe%d>;\n' % kw.get('group', 0)
    for v, n in enumerate(kw.get('enum', [])):
        b += '\t\tlv2:scalePoint [ rdfs:label "%s"; rdf:value %d ];\n' % (n, v)
    result.append(b)

open('/tmp/ports.ttl','w').write(''.join(result) + '\t].')

# --- the expected order, for checking and for the C struct
open('/tmp/ordre.txt','w').write('\n'.join([p[1] for p in P]))
print("%d ports generated, 7 through %d, %d in total with the original 7"
      % (len(P), 7 + len(P) - 1, 7 + len(P)))
