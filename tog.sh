#!/bin/bash
# tog.sh NAME PATCH : freeze at 60, toggle patch every 12 frames, compare end-of-phase frames
R=${IPU7_RE_SCRATCH:-/tmp/ipu7-re}/cmpd; N=$1; P=$2
cd ~/Projects/ipu7-re && env $3 LIVE_FREEZE=60 LIVE_TOGGLE=12 LIVE_PATCH="$P" timeout 25 ./live2 bundles graph 205 2>/dev/null | python3 -W ignore -c "
import sys,numpy as np
from PIL import Image
W,H=1920,1080; FS=W*H*3//2; i=0; fr={}
want=set(60+12*k+11 for k in range(12))
while True:
    d=sys.stdin.buffer.read(FS)
    if len(d)<FS: break
    if i in want: fr[i]=np.frombuffer(d,np.uint8)[:W*H].reshape(H,W).astype(np.float32)
    i+=1
ks=sorted(fr); off=[fr[k] for k in ks if ((k-60)//12)%2==0]; on=[fr[k] for k in ks if ((k-60)//12)%2==1]
# pair each ON with the mean of neighbouring OFFs
diffs=[]; noise=[]
for j in range(min(len(on),len(off)-1)):
    ref=(off[j]+off[j+1])/2; diffs.append(on[j]-ref); noise.append(off[j+1]-off[j])
D=np.mean(diffs,0); Nz=np.mean(np.abs(noise))
A=np.mean(off,0)
print(f'$N: mean|on-off| {np.mean(np.abs(D)):.2f} (off-vs-off drift {Nz:.2f})')
for lo,hi in ((16,50),(50,100),(100,150),(150,200),(200,236)):
    m=(A>=lo)&(A<hi)
    if m.mean()>0.005: print(f'   off Y {lo:3}-{hi:3}: on-off {D[m].mean():+6.2f}  ({m.mean()*100:4.1f}% px)')
def hp(y):
    k=np.ones(5)/5; b=np.apply_along_axis(lambda r: np.convolve(r,k,'same'),1,y); b=np.apply_along_axis(lambda c: np.convolve(c,k,'same'),0,b); return y-b
def metrics(fs):
    h=[hp(f[::2,::2]) for f in fs]; A2=A[::2,::2]
    flat=[np.abs(x)[(np.abs(hp(A2))<3)&(A2>40)].mean() for x in h]      # residual texture in flat areas = noise
    edge=[np.abs(x)[np.abs(hp(A2))>12].mean() for x in h]               # high-pass on edges = detail
    return np.mean(flat), np.mean(edge)
fo,eo=metrics(off[:4]); fn,en=metrics(on[:4])
print(f'   noise (flat-area HF): off {fo:.2f} on {fn:.2f} ({(fn/fo-1)*100:+.0f}%)   detail (edge HF): off {eo:.2f} on {en:.2f} ({(en/eo-1)*100:+.0f}%)')
np.save('$R/tog_$N.npy',D); Image.fromarray(np.clip(on[-1],0,255).astype(np.uint8)).resize((640,360)).save('$R/tog_${N}_on.png'); Image.fromarray(np.clip(off[-1],0,255).astype(np.uint8)).resize((640,360)).save('$R/tog_${N}_off.png')
"
