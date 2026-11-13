#!/usr/bin/env python3
"""Plan 0.8.6 P0: overlay, transition and decal counts over the P2CE VMF corpus ($APEX_P2CE_DIR)."""
import os, re, sys, collections
root = os.path.join(os.environ['APEX_P2CE_DIR'], 'sdk_content/maps')
tok = re.compile(r"\"((?:[^\"\\\\]|\\\\.)*)\"|([{}])|([A-Za-z_]\w*)")
def parse(text):
    stack=[('root',[])]; key=None
    for m in tok.finditer(text):
        s, b = (m.group(1) if m.group(1) is not None else m.group(3)), m.group(2)
        if b == '{':
            node=(key,[]); stack[-1][1].append(node); stack.append(node); key=None
        elif b == '}':
            stack.pop(); key=None
        elif key is None: key=s
        else: stack[-1][1].append((key,s)); key=None
    return stack[0]
def blocks(node,name):
    for c in node[1]:
        if isinstance(c[1],list) and c[0] and c[0].lower()==name: yield c
def val(node,k):
    for c in node[1]:
        if not isinstance(c[1],list) and c[0].lower()==k.lower(): return c[1]
    return None
def walk_solids(node):
    for c in node[1]:
        if isinstance(c[1],list):
            if c[0] and c[0].lower()=='solid': yield c
            else: yield from walk_solids(c)
maps=sorted(os.path.join(d,f) for d,_,fs in os.walk(root) for f in fs if f.endswith('.vmf'))
rows=[]; faces_hist=collections.Counter(); totals=collections.Counter(); classes=collections.Counter()
for p in maps:
    t=open(p,encoding='latin-1').read()
    r=parse(t)
    world=next(blocks(r,'world'),None)
    disp=set(); sidecount=0
    for s in walk_solids(r):
        for side in blocks(s,'side'):
            sidecount+=1
            if next(blocks(side,'dispinfo'),None): disp.add(val(side,'id'))
    def allents(n):
        for c in n[1]:
            if isinstance(c[1],list):
                if c[0] and c[0].lower()=='entity': yield c
                elif c[0] and c[0].lower()=='hidden': yield from allents(c)
    ents=list(allents(r))
    hiddenov=sum(1 for c in r[1] if isinstance(c[1],list) and c[0]=='hidden' for e in blocks(c,'entity') if (val(e,'classname') or '').lower()=='info_overlay')
    totals['hidden_overlays']+=hiddenov
    ov=[e for e in ents if (val(e,'classname') or '').lower()=='info_overlay']
    tr=[e for e in ents if (val(e,'classname') or '').lower()=='info_overlay_transition']
    dec=[e for e in ents if (val(e,'classname') or '').lower()=='infodecal']
    cub=[e for e in ents if (val(e,'classname') or '').lower()=='env_cubemap' and (val(e,'sides') or '').strip()]
    nf=[len((val(e,'sides') or '').split()) for e in ov]
    ondisp=sum(1 for e in ov if any(x in disp for x in (val(e,'sides') or '').split()))
    for n in nf: faces_hist[n]+=1
    totals['maps']+=1; totals['overlays']+=len(ov); totals['transitions']+=len(tr); totals['decals']+=len(dec); totals['ondisp']+=ondisp; totals['dispsides']+=len(disp)
    totals['cubemap_sides']+=len(cub)
    missing=sum(1 for e in ov for x in (val(e,'sides') or '').split() if not re.search(r'"id" "%s"'%re.escape(x), t))
    for e in ov: classes[(val(e,'material') or '')]+=1
    if ov or tr or dec:
        rows.append((os.path.relpath(p,root),len(ov),max(nf) if nf else 0,ondisp,len(tr),len(dec),missing, sum(1 for e in ov if not (val(e,'sides') or '').strip())))
print('maps',totals)
print('faces per overlay',sorted(faces_hist.items()))
for r in rows: print(r)
print('materials', len(classes))
