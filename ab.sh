#!/bin/bash
# ab.sh NAME [env...]  -> cmpd/NAME.jpg (frame 120, full res) + stats
R=${IPU7_RE_SCRATCH:-/tmp/ipu7-re}/cmpd; N=$1; shift
cd ~/Projects/ipu7-re && env "$@" timeout 25 ./live2 bundles graph 121 2>/dev/null | python3 -W ignore -c "
import sys,numpy as np
from PIL import Image
W,H=1920,1080; FS=W*H*3//2; i=0
while True:
    d=sys.stdin.buffer.read(FS)
    if len(d)<FS: break
    if i==120:
        a=np.frombuffer(d,np.uint8); y=a[:W*H].reshape(H,W).astype(np.float32); uv=a[W*H:].reshape(H//2,W//2,2).astype(np.float32)
        np.save('$R/$N.y.npy', y)
        y2=(y-16)*255/219; u=np.repeat(np.repeat(uv[:,:,0],2,0),2,1)-128; v=np.repeat(np.repeat(uv[:,:,1],2,0),2,1)-128
        rgb=np.clip(np.stack([y2+1.79*v, y2-0.21*u-0.53*v, y2+2.11*u],-1),0,255).astype(np.uint8)
        Image.fromarray(rgb).save('$R/$N.jpg')
        p=np.percentile(y,[1,5,25,50,75,95,99]).round(0).tolist(); print('$N Y pct',p)
    i+=1"
