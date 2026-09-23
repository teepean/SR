import sys,csv
from collections import Counter
def load(d):
    data={int(r[0],16):r for r in csv.reader(open(d+'/data_refs.csv'))}
    heads={int(r[0],16):r[2] for r in csv.reader(open(d+'/heads.csv'))}
    funcs={int(r[0],16) for r in csv.reader(open(d+'/funcs.csv'))}
    segs=[(r[0],int(r[1],16),int(r[2],16)) for r in csv.reader(open(d+'/segments.csv'))]
    return data,heads,funcs,segs
def classify(d, bss_start=None, bs=None, aligned_data=False):
    data,heads,funcs,segs=load(d)
    def seg(a):
        for n,s,e in segs:
            if s<=a<e: return n
    acc={};rej={}
    for a,r in data.items():
        v=int(r[1],16); why=None
        st=seg(v); ss=seg(a)
        # (IDA's offset flag is not trusted: it marked the string "NEC" as an offset in i76.exe)
        if (v&0xffff)==0: why='low16zero'
        elif r[3]=='s': why='string'
        elif ss!='.text' and st=='.text' and v not in funcs and all(0x41<=((v>>(8*k))&0xff)<=0x5a for k in range(3)): why='text-tag'
        elif aligned_data and ss!='.text' and a%4: why='unaligned-data'
        elif bss_start and v>=bss_start and v not in heads and all(0x20<=((v>>(8*k))&0xff)<0x7f for k in range(3)): why='bss-ascii'
        elif ss=='.text' and r[3]=='d' and r[5]=='-' and a%4: why='text-data-misaligned'
        elif all(0x20<=((v>>(8*k))&0xff)<0x7f for k in range(3)) and v not in heads and v not in funcs and (v&3)!=0: why='ascii'
        elif r[5] in ('float','double','qword','tbyte'): why='fp-item'
        elif r[5]=='dword' and int(r[4])%4: why='dword-misaligned'
        elif r[5]=='word' and int(r[4])%2: why='word-misaligned'
        elif st=='.text' and v not in heads: why='text-nonhead'
        elif st=='.text' and heads.get(v)!='c': why='text-noncode'
        if not why and bs is not None and st=='.text' and len(r)>7 and r[7]=='0' \
                and not (r[3]=='d' and (ss=='.text' or r[5]=='struct')) and not bs.is_block_start(v):
            why='text-mid-block-noxref'
        if why: rej[a]=(v,why)
        else: acc[a]=(v,'ok')
    # overlaps among accepted: group overlapping candidates; keep a single 4-aligned
    # candidate whose target is a known head, otherwise reject the whole group
    ks=sorted(acc); groups=[]; cur=[]
    for a in ks:
        if cur and a-cur[-1]<4: cur.append(a)
        else:
            if len(cur)>1: groups.append(cur)
            cur=[a]
    if len(cur)>1: groups.append(cur)
    for g in groups:
        good=[a for a in g if a%4==0 and acc[a][0] in heads]
        keep=None
        if len(good)==1:
            k=good[0]
            if all(abs(o-k)>=4 or o==k for o in good): keep=k
        for a in g:
            if a!=keep: rej[a]=acc[a][0],'overlap'
    for a in rej: acc.pop(a,None)
    # rescue: an aligned rejected candidate inside a regular table (accepted neighbors at the same
    # stride on both sides or two on one side, pointing close to the same target) is a pointer too
    rescue_reasons=('bss-ascii','ascii','low16zero')
    for a in sorted(rej):
        v,why=rej[a]
        if why not in rescue_reasons or a%4: continue
        for stride in (8,12,16,20,24,28,32):
            def near(x):
                return x in acc and abs(acc[x][0]-v) <= 0x100
            if (near(a-stride) and near(a+stride)) or (near(a-stride) and near(a-2*stride)) or (near(a+stride) and near(a+2*stride)):
                acc[a]=(v,'rescued'); break
    for a in [x for x in acc if acc[x][1]=='rescued']: rej.pop(a,None)
    return data,acc,rej,seg,heads
if __name__=='__main__':
    data,acc,rej,seg,heads=classify(sys.argv[1])
    if len(sys.argv)>2:
        truth={}
        for l in open(sys.argv[2]):
            p=l.strip().split(',')
            if len(p)>=2 and p[0]: truth[int(p[0],16)]=int(p[1],16)
        print("acc",len(acc),"TP",sum(a in truth for a in acc),"FP",sum(a not in truth for a in acc))
        print("rej",len(rej),"FN(rejected truth)",Counter(rej[a][1] for a in rej if a in truth), "TN",Counter(rej[a][1] for a in rej if a not in truth))
        print("FP detail",Counter((seg(a),seg(acc[a][0]),heads.get(acc[a][0],'-')) for a in acc if a not in truth))
        for a in sorted(a for a in acc if a not in truth)[:40]: print("  FP %x -> %x %s"%(a,acc[a][0],data[a][3:]))
