# TLS MQTT sink for load tests: TLS handshake, answer CONNECT with CONNACK, close after 0.3 s
import socket, ssl, sys, threading, time
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[1], sys.argv[2])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', 8883)); srv.listen(8)
end = time.time() + float(sys.argv[3]); srv.settimeout(1)
count = {}
def handle(c, a):
    try:
        c.settimeout(10); s = ctx.wrap_socket(c, server_side=True)
        s.settimeout(2); d = s.recv(512)
        if d and d[0] >> 4 == 1: s.sendall(b'\x20\x02\x00\x00')
        time.sleep(0.3); s.close()
        count[a[0]] = count.get(a[0], 0) + 1
        print('%.1f handshake %s #%d' % (time.time(), a[0], count[a[0]]), flush=True)
    except Exception as e:
        print('%.1f error %s %s' % (time.time(), a[0], e), flush=True)
        c.close()
while time.time() < end:
    try: c, a = srv.accept()
    except socket.timeout: continue
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
