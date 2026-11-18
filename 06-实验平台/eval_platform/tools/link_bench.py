#!/usr/bin/env python3
import socket,sys,time,json
role,scenario=sys.argv[1],sys.argv[2]
PORT=int(sys.argv[3]); SECS=5.0; BLOCK=65536
if role=='server':
    s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    s.bind(('0.0.0.0',PORT)); s.listen(3); s.settimeout(20)
    try: c,_=s.accept()
    except Exception: print(json.dumps({'error':'no client'})); sys.exit(1)
    c.settimeout(10); total=0; t0=time.monotonic()
    try:
        while time.monotonic()-t0<SECS+4:
            d=c.recv(BLOCK)
            if not d: break
            total+=len(d)
            if scenario=='slow_receiver': time.sleep(0.002)
            if scenario=='delay_5ms': time.sleep(0.005)
    except Exception: pass
    c.close(); s.close()
    print(json.dumps({'role':'server','scenario':scenario,'bytes':total}))
else:
    host=sys.argv[4]
    s=socket.create_connection((host,PORT),timeout=8); s.settimeout(8)
    payload=b'x'*BLOCK; total=0; t0=time.monotonic(); target=10*1024*1024
    while time.monotonic()-t0<SECS:
        s.sendall(payload); total+=len(payload)
        if scenario=='rate_10MBps':
            expected=total/target
            if expected>time.monotonic()-t0: time.sleep(expected-(time.monotonic()-t0))
        if scenario=='delay_5ms': time.sleep(0.005)
    dt=time.monotonic()-t0; s.close()
    print(json.dumps({'role':'client','scenario':scenario,'bytes':total,'seconds':round(dt,2),'mbps':round(total*8/dt/1e6,2)}))
