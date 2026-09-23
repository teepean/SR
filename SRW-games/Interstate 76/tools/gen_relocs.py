#!/usr/bin/env python3
# Build relocations.csv for a relocation-stripped PE from ida_export.py output.
# usage: gen_relocs.py <ida_out_dir> <imagebase_hex> <out_csv> [include.csv|-] [exclude.txt|-] [bss_start_hex] [exe]
import sys, csv
from classify import classify
from blockstart import BS
import pefile
d=sys.argv[1]; ib=int(sys.argv[2],16); out=sys.argv[3]
inc=sys.argv[4] if len(sys.argv)>4 and sys.argv[4]!='-' else None; exc=sys.argv[5] if len(sys.argv)>5 and sys.argv[5]!='-' else None
bss=int(sys.argv[6],16) if len(sys.argv)>6 else None
exe=sys.argv[7] if len(sys.argv)>7 else None
bs=None
if exe:
    pe=pefile.PE(exe)
    bs=BS(exe,d,[(ib+x.VirtualAddress,x.PointerToRawData,x.SizeOfRawData) for x in pe.sections])
data,acc,rej,seg,heads=classify(d,bss,bs)
rel={}
for r in csv.reader(open(d+'/code_refs.csv')):
    a=int(r[0],16); v=int(r[1],16)
    rel[a]=(v,'i' if v<ib+0x1000 else '')
for a,(v,why) in acc.items():
    if a not in rel: rel[a]=(v,'')
if inc:
    for l in open(inc):
        l=l.split('#')[0].strip()
        if not l: continue
        p=l.split(','); rel[int(p[0],0)]=(int(p[1],0),p[2].strip() if len(p)>2 else '')
if exc:
    for l in open(exc):
        l=l.split('#')[0].strip()
        if l: rel.pop(int(l,0),None)
# overlap sanity
ks=sorted(rel)
for x,y in zip(ks,ks[1:]):
    if y-x<4: print("WARNING overlapping relocations %x %x"%(x,y))
with open(out,'w') as f:
    for a in ks:
        v,t=rel[a]
        f.write("0x%x,0x%x,%s\n"%(a,v,'imagebase' if t=='i' else ''))
with open(out+'.rejected.txt','w') as f:
    for a in sorted(rej):
        f.write("0x%x,0x%x,%s,%s\n"%(a,rej[a][0],rej[a][1],','.join(data[a][3:])))
print("relocations",len(rel),"code",sum(1 for a in rel if rel[a][1]=='' ) ,"rejected data",len(rej))
