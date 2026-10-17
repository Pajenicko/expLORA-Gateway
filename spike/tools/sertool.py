# Serial logger with command injection: sertool.py <port> <logfile> <cmdfile> [seconds]
# Each received line is prefixed with host time. Text appended to <cmdfile> is sent (then the file is emptied).
import serial, sys, time, os
port, out, cmdf = sys.argv[1], sys.argv[2], sys.argv[3]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = os.environ.get('DTR') == '1'; s.rts = False
s.open()
open(cmdf, 'a').close()
end = time.time() + secs
buf = b''
with open(out, 'a', buffering=1) as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                f.write('%.3f %s\n' % (time.time(), line.decode('utf-8', 'replace').rstrip('\r')))
        try:
            if os.path.getsize(cmdf) > 0:
                c = open(cmdf).read(); open(cmdf, 'w').close()
                for l in c.splitlines():
                    if l.strip():
                        s.write((l.strip() + '\n').encode()); f.write('%.3f >>> %s\n' % (time.time(), l.strip()))
        except OSError:
            pass
