"""Poll PCSX2 via PINE: VU1 data memory + VIF1 regs, twice a second. usage: pine_poll_vu1.py <seconds> <outdir>"""
import sys, time, struct, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))  # shared tools (pine_read)
from pine_read import Pine
secs = float(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
p = Pine()
VIF1 = {'stat':0x10003C00,'cycle':0x10003C40,'mode':0x10003C50,'mask':0x10003C70,'base':0x10003CA0,'ofst':0x10003CB0,'tops':0x10003CC0,'itop':0x10003CD0,'top':0x10003CE0}
t0 = time.time(); n = 0
while time.time() - t0 < secs:
    t = time.time() - t0
    regs = {k: struct.unpack('<I', p.read(a, 4)[:4])[0] for k, a in VIF1.items()}
    vu = p.read(0x1100C000, 0x4000)
    stub = struct.unpack('<I', p.read(0x49DE50, 4)[:4])[0]
    name = os.path.join(out, 'vu1data_%05.1f.bin' % t)
    open(name, 'wb').write(vu)
    print('%5.1fs stub=%08x ' % (t, stub) + ' '.join('%s=%x' % kv for kv in regs.items()), flush=True)
    n += 1
    time.sleep(max(0.0, 0.5 - (time.time() - t0 - t)))
print('snapshots:', n)
