import re,bisect,collections,sys
syms=[]
for l in open('replay.map',errors='replace'):
    m=re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s',l)
    if m: syms.append((int(m.group(2),16),m.group(1)))
syms.sort(); base=0x140000000
addrs=[a for a,_ in syms]
c=collections.Counter(); n=0
for l in open('replay.rip'):
    v=int(l,16)+base; i=bisect.bisect_right(addrs,v)-1
    c[syms[i][1] if i>=0 else '?']+=1; n+=1
print(n,'samples')
for k,v in c.most_common(int(sys.argv[1]) if len(sys.argv)>1 else 30): print(f'{100*v/n:5.1f}% {k[:120]}')
