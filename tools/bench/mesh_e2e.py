"""Warthog mesh end-to-end suite, 3 boards."""
import time, re, sys, serial, serial.tools.list_ports as lp
from meshtastic.protobuf import mesh_pb2
IDS={'F8738D':'WTHG-0272A1F8738D','81BA51':'WTHG-021BF681BA51','F8823D':'WTHG-0272A1F8823D'}
def resolve():
    pm={}
    for p in lp.comports():
        if p.vid==0x303a and p.serial_number: pm.setdefault(p.serial_number,[]).append(p.device)
    out={}
    for k,sn in IDS.items():
        for d in pm.get(sn,[]):
            try:
                s=serial.Serial(d,115200,timeout=1); time.sleep(0.35); s.reset_input_buffer()
                s.write(b'AT\r\n'); s.flush(); time.sleep(0.7)
                r=s.read(200).decode(errors='replace'); s.close()
                if 'OK' in r: out[k]=d; break
            except Exception: pass
    return out
wait=int(sys.argv[1]) if len(sys.argv)>1 else 55
print("settling %ds..."%wait); time.sleep(wait)
P=resolve(); print("boards up: %s\n"%", ".join(sorted(P)))
def q(k, cmd, w=1.4):
    """Query a board and drain until the AT response terminator.

    Not a fixed-size read: the firmware also emits ESP_LOG lines on this port,
    and a fixed read returns whichever bytes arrived FIRST -- log spam -- while
    the reply is still in the buffer. That made working commands look like
    unreachable boards. Read until OK/ERROR or a deadline instead.

    A board that falls off USB returns "" so the caller records a miss and the
    run continues; these boards do that, and it is a bench fact, not a mesh
    result."""
    try:
        s = serial.Serial(P[k], 115200, timeout=0.4)
        time.sleep(0.3); s.reset_input_buffer()
        s.write(cmd + b'\r\n'); s.flush()
        deadline = time.time() + w + 6.0
        buf = b''
        while time.time() < deadline:
            chunk = s.read(1024)
            if chunk:
                buf += chunk
                tail = buf[-400:]
                if b'\r\nOK\r\n' in tail or b'ERROR' in tail:
                    # Give a multi-line reply a moment to finish arriving.
                    time.sleep(0.25); buf += s.read(4096)
                    break
        s.close()
        return buf.decode(errors='replace')
    except Exception as e:
        print("   !! %s unreachable (%s)" % (k, type(e).__name__)); return ""
def one(k,cmd,tag):
    m=re.search(r'\+'+tag+r': (.*)',q(k,cmd)); return m.group(1).strip() if m else '?'
RESULTS=[]
def check(name, ok, detail): RESULTS.append((name,ok,detail)); print("  [%s] %s -- %s"%("PASS" if ok else "FAIL",name,detail))

print("== 1. peering ==")
for k in sorted(P):
    links=one(k,b'AT+MPMPEERS?','MPMPEERS'); n=links.count('estab=1')
    check("peering/%s"%k, n==len(P)-1, "%d/%d links established"%(n,len(P)-1))

print("\n== 2. datapath peers ==")
IP={}
for k in sorted(P):
    IP[k]=(lambda m: m.group(1) if m else '?')(re.search(r'ip=(\S+)',q(k,b'AT+STATUS?')))
    c=int(re.search(r'count=(\d+)',one(k,b'AT+PEERS?','PEERS')).group(1))
    check("peers/%s"%k, c==len(P)-1, "count=%d ip=%s"%(c,IP[k]))

print("\n== 3. unicast ICMP, all ordered pairs (warm) ==")
for a in sorted(P):
    for b in sorted(P):
        if a==b: continue
        q(a,('AT+MPING=%s,1'%IP[b]).encode(),w=4)   # warm ARP
        o=q(a,('AT+MPING=%s,5'%IP[b]).encode(),w=9)
        m=re.search(r'(\d+) sent, (\d+) received',o)
        if not m:
            # No answer from the SENDER's console is a bench failure, not a
            # mesh result -- reporting it as 0/0 loss blames the radio for a
            # board that fell off USB.
            check("icmp/%s->%s"%(a,b), False, "no answer from %s (board unreachable?)"%a)
            continue
        s_,r_=int(m.group(1)),int(m.group(2))
        check("icmp/%s->%s"%(a,b), r_==s_ and s_>0, "%d/%d"%(r_,s_))

print("\n== 4. multicast 239.0.0.69:4403 -- each sender reaches all peers ==")
for k in sorted(P): q(k,b'AT+MCAST=1')
time.sleep(3)
for src in sorted(P):
    base={k:int(re.search(r'rx=(\d+)',one(k,b'AT+MCAST?','MCAST')).group(1)) for k in P}
    for i in range(5): q(src,('AT+MSEND=m%d'%i).encode(),w=0.5)
    time.sleep(2)
    got={k:int(re.search(r'rx=(\d+)',one(k,b'AT+MCAST?','MCAST')).group(1))-base[k] for k in P}
    rec={k:v for k,v in got.items() if k!=src}
    check("mcast/%s->all"%src, all(v>=5 for v in rec.values()), " ".join("%s=+%d"%(k,v) for k,v in sorted(rec.items())))

print("\n== 5. real Meshtastic MeshPackets across the mesh ==")
def build(pid,t):
    mp=mesh_pb2.MeshPacket(); mp.to=0xFFFFFFFF; setattr(mp,"from",0x0A0B0C0D)
    mp.id=pid; mp.channel=8; mp.hop_limit=3; mp.encrypted=("ENC:"+t).encode()
    return mp.SerializeToString()
src=sorted(P)[0]; dsts=[k for k in sorted(P) if k!=src]
okc=0
for i in range(3):
    wire=build(0x4000+i,"e2e halow %d"%i)
    q(src,b'AT+MINJECT='+wire.hex().encode()); time.sleep(1.2)
    hits=0
    for d in dsts:
        m=re.search(r'\+MUDPLAST: src=(\S+) len=(\d+) hex=([0-9a-f]*)',q(d,b'AT+MUDPLAST?'))
        if not m: continue
        got=bytes.fromhex(m.group(3)); mp=mesh_pb2.MeshPacket()
        try:
            mp.ParseFromString(got)
            if got==wire and mp.id==0x4000+i and mp.WhichOneof("payload_variant")=="encrypted": hits+=1
        except Exception: pass
    if hits==len(dsts): okc+=1
check("meshpacket/%s->all"%src, okc==3, "%d/3 byte-exact on all %d peers"%(okc,len(dsts)))

print("\n" + "="*58)
p=sum(1 for _,o,_ in RESULTS if o); print("E2E: %d/%d PASS"%(p,len(RESULTS)))
for n,o,d in RESULTS:
    if not o: print("   FAILED: %s -- %s"%(n,d))
