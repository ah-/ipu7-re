import glob,re,sys,json,numpy as np
secs=json.load(open('lbff_sections.json'))
def frames(exp,node=0,term=0):
    fs=sorted(glob.glob(f'exp/{exp}/t*_n{node}_term{term}.bin'))
    return [np.fromfile(f,dtype=np.uint8) for f in fs]
B=frames('base')+frames('base2')
noise=np.zeros_like(B[0],dtype=bool)
for f in B[1:]: noise|=(f!=B[0])
ref=B[-1]
def sec_of(i):
    for n,dev,sz,off in secs:
        if off<=i<off+sz: return f"{n}@{dev:#x}"
    return "?"
print("noisy bytes in baseline:", int(noise.sum()), "in", sorted({sec_of(i) for i in np.nonzero(noise)[0]}))
for exp in sys.argv[1:]:
    d=frames(exp)[-1]
    ch=np.nonzero((d!=ref)&~noise)[0]
    cnt={}
    for i in ch: s=sec_of(i); cnt[s]=cnt.get(s,0)+1
    print(f"{exp:7}", ", ".join(f"{k}:{v}" for k,v in sorted(cnt.items(),key=lambda x:-x[1])) or "(none)")
