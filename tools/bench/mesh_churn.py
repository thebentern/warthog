"""Node leave/rejoin: do the survivors keep working, and does the node come back?"""
import time, re, subprocess, serial, serial.tools.list_ports as lp
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
                if 'OK' in s.read(200).decode(errors='replace'): out[k]=d; s.close(); break
                s.close()
            except Exception: pass
    return out
time.sleep(50)
P=resolve()
def q(k,cmd,w=1.4):
    s=serial.Serial(P[k],115200,timeout=2); time.sleep(0.3); s.reset_input_buffer()
    s.write(cmd+b'\r\n'); s.flush(); time.sleep(w)
    return s.read(4500).decode(errors='replace')
def ip(k): return re.search(r'ip=(\S+)',q(k,b'AT+STATUS?')).group(1)
def ping(a,b,n=4):
    o=q(a,('AT+MPING=%s,%d'%(ip_[b],n)).encode(),w=n*2+3)
    m=re.search(r'(\d+) sent, (\d+) received',o); return "%s/%s"%(m.group(2),m.group(1)) if m else '?'
def links(k):
    return one(k,b'AT+MPMPEERS?').count('estab=1')
def one(k,cmd):
    m=re.search(r'\+\w+: (.*)',q(k,cmd)); return m.group(1).strip() if m else ''
ip_={k:ip(k) for k in P}
VICTIM='F8823D'; SURV=[k for k in sorted(P) if k!=VICTIM]
print("baseline: links %s | %s->%s %s"%({k:links(k) for k in sorted(P)}, SURV[0],SURV[1],ping(SURV[0],SURV[1])))
print("\n-- powering %s OFF --"%VICTIM)
subprocess.run("uhubctl -l 0-1.3 -p 1 -a off",shell=True,capture_output=True); time.sleep(45)
print("survivors: links %s | %s->%s %s"%({k:links(k) for k in SURV}, SURV[0],SURV[1],ping(SURV[0],SURV[1])))
print("\n-- powering %s back ON --"%VICTIM)
subprocess.run("uhubctl -l 0-1.3 -p 1 -a on",shell=True,capture_output=True); time.sleep(70)
P=resolve(); ip_={k:ip(k) for k in P}
print("rejoined:  links %s"%{k:links(k) for k in sorted(P)})
for a in sorted(P):
    for b in sorted(P):
        if a!=b: print("   %s->%s %s"%(a,b,ping(a,b)))
