import glob,re,sys,numpy as np
E='exp'
def last(exp):
    out={}
    for f in glob.glob(f'{E}/{exp}/t*_n*_term*.bin'):
        m=re.search(r't(\d+)_n(\d+)_term(\d+)',f); k=(int(m[2]),int(m[3])); t=int(m[1])
        if k not in out or t>out[k][0]: out[k]=(t,f)
    return {k:np.fromfile(v[1],dtype=np.uint8) for k,v in out.items()}
def ranges(idx):
    r=[];s=p=None
    for i in idx:
        if s is None: s=p=i
        elif i<=p+8: p=i
        else: r.append((s,p)); s=p=i
    if s is not None: r.append((s,p))
    return r
base,base2=last('base'),last('base2')
noise={k:(base[k]!=base2[k]) for k in base}
print('noise (base vs base2):', {f'{k[0]}.{k[1]}':int(v.sum()) for k,v in sorted(noise.items()) if v.sum()})
for exp in sys.argv[1:]:
    d=last(exp); res=[]
    for k in sorted(base):
        if k not in d: continue
        ch=np.nonzero((base[k]!=d[k]) & ~noise[k])[0]
        if len(ch): res.append(f"{k[0]}.{k[1]}:{len(ch)}B@"+",".join(f"{a:#x}-{b:#x}" for a,b in ranges(ch)[:6])+("…" if len(ranges(ch))>6 else ""))
    print(f'{exp:7}', ' | '.join(res) if res else '(no change)')
