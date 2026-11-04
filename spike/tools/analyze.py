# analyze.py <dir> [phase...] : per phase and gateway: err/lag/notify/launch percentiles from "SPK tx" lines,
# counters from the stats line, sensor hit rate from "P seq" lines
import re, sys, os
d = sys.argv[1]
phases = []
cur = {}
for line in open(os.path.join(d, 'phases.log')):
    t, kind, label = line.split()
    t = float(t)
    if kind == 'START': cur = {'label': label, 'start': t}
    elif kind == 'PING': cur['ping'] = t
    elif kind == 'END': cur['end'] = t; phases.append(cur)

def pct(v, p):
    if not v: return None
    v = sorted(v); return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]

def summarize(vals):
    if not vals: return 'n=0'
    return 'n=%d min=%d p50=%d p90=%d p99=%d max=%d' % (len(vals), min(vals), pct(vals, 50), pct(vals, 90), pct(vals, 99), max(vals))

def gw_lines(name, t0, t1):
    out = []
    path = os.path.join(d, name + '.log')
    if not os.path.exists(path):
        path = os.path.join(d, name + '-spk.log')  # filtered copy in spike/data
    for line in open(path, errors='replace'):
        try: t = float(line.split(' ', 1)[0])
        except ValueError: continue
        if t0 <= t <= t1: out.append(line)
    return out

def parse_tx(lines, kind):
    rows = []
    for l in lines:
        if 'SPK tx ' + kind in l:
            kv = dict(re.findall(r'(\w+)=(-?\d+)', l))
            rows.append({k: int(v) for k, v in kv.items()})
    return rows

only = sys.argv[2:] 
for ph in phases:
    if only and ph['label'] not in only: continue
    print('=== %s (beacon %.0f s, ping %.0f s)' % (ph['label'], ph['ping'] - ph['start'], ph['end'] - ph['ping']))
    for gw in ('pro', 'lite'):
        for part, t0, t1, kind in (('beacon', ph['start'], ph['ping'], 'beacon'), ('ping', ph['ping'], ph['end'], 'pong')):
            rows = parse_tx(gw_lines(gw, t0, t1 + 2), kind)
            if not rows: continue
            print('  %-4s %-6s err    %s' % (gw, part, summarize([r['err'] for r in rows])))
            print('  %-4s %-6s start  %s' % (gw, part, summarize([r['start'] for r in rows])))
            print('  %-4s %-6s lag    %s' % (gw, part, summarize([r['lag'] for r in rows if r['lag'] != 0])))
            print('  %-4s %-6s launch %s  prep %s' % (gw, part, summarize([r['launch'] for r in rows]), summarize([r['prep'] for r in rows])))
            if kind == 'pong':
                print('  %-4s %-6s notify %s' % (gw, part, summarize([r['notify'] for r in rows])))
            busy = sum(1 for r in rows if r.get('busy'))
            bad = sum(1 for r in rows if r.get('state', 0) != 0)
            print('  %-4s %-6s busyAtSwitch=%d stateErr=%d' % (gw, part, busy, bad))
    # sensor
    res = {0: {}, 1: {}}; offs = {0: [], 1: []}; wakes = []
    for line in open(os.path.join(d, 'sensor.log'), errors='replace'):
        if ' P seq=' not in line: continue
        t = float(line.split(' ', 1)[0])
        if not (ph['ping'] + 5 <= t <= ph['end']): continue
        kv = dict(re.findall(r'(\w+)=(-?\w+)', line))
        tg = int(kv['tgt']); r = kv['res']
        res[tg][r] = res[tg].get(r, 0) + 1
        if r == 'HIT': offs[tg].append(int(kv['off'])); wakes.append(int(kv['wake']))
    for tg, name in ((0, 'pro'), (1, 'lite')):
        n = sum(res[tg].values())
        if n:
            print('  sensor->%-4s pings=%d %s hit=%.1f%%  offset %s' % (name, n, res[tg], 100.0 * res[tg].get('HIT', 0) / n, summarize(offs[tg])))
    if wakes: print('  sensor wake err %s' % summarize(wakes))
