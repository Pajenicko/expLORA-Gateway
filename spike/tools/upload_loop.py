# Repeated firmware uploads that are cut off after a few seconds (never complete):
# upload_loop.py <seconds> <ip> <jar> <bin> [<ip> <jar> <bin> ...]
import subprocess, sys, threading, time
end = time.time() + float(sys.argv[1])
def run(ip, jar, binf):
    n = 0
    while time.time() < end:
        r = subprocess.run(['curl', '-s', '-o', '/dev/null', '-w', '%{http_code}', '-b', jar, '--limit-rate', '80k',
                            '--max-time', '12', '-F', 'bin=@' + binf, 'http://%s/firmware' % ip],
                           capture_output=True, text=True)
        n += 1
        print('%.1f upload %s #%d -> %s' % (time.time(), ip, n, r.stdout), flush=True)
        time.sleep(1)
a = sys.argv[2:]
ts = [threading.Thread(target=run, args=(a[i], a[i + 1], a[i + 2])) for i in range(0, len(a), 3)]
[t.start() for t in ts]; [t.join() for t in ts]
