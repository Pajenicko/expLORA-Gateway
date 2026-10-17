# phase.py <label> <beacon s> <ping s> [gateways...]
import sys, time, os
d = os.path.dirname(os.path.abspath(__file__))
label, b, p = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
gws = sys.argv[4:] or ['pro', 'lite']
def cmd(c):
    for g in gws:
        open(os.path.join(d, g + '.cmd'), 'a').write(c + '\n')
def mark(k):
    open(os.path.join(d, 'phases.log'), 'a').write('%d %s %s\n' % (time.time(), k, label))
mark('START'); cmd('spike-reset %s-beacon' % label); cmd('spike-beacon on')
time.sleep(b)
cmd('spike-beacon off'); time.sleep(3); cmd('spike-reset %s-ping' % label); mark('PING')
time.sleep(p)
cmd('spike-reset %s-end' % label); mark('END')
