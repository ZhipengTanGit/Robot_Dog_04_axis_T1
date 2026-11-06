"""Offline diagnostic render of body exterior color boundaries (no CAD/UI IO)."""
from pathlib import Path
import json
import numpy as np
from PIL import Image, ImageDraw

root=Path(__file__).resolve().parents[1]
model=json.loads((root/'dist/assets/model.json').read_text(encoding='utf-8'))
blob=(root/'dist/assets/model.bin').read_bytes()
body=[m for m in model['meshes'] if m['part'].startswith('打印件_身体_')]
look=np.array([1.,-.7,1.3]);look/=np.linalg.norm(look)
right=np.cross(look,[0.,1.,0.]);right/=np.linalg.norm(right)
up=np.cross(right,look)
triangles=[]
for m in body:
    p=np.frombuffer(blob,'<f4',m['positions']['count']*3,m['positions']['byteOffset']).reshape(-1,3)
    for face in m['faces']:
        start,end=face['firstVertex'],face['firstVertex']+face['count']
        for t in p[start:end].reshape(-1,3,3):
            n=np.cross(t[1]-t[0],t[2]-t[0]);n/=max(np.linalg.norm(n),1e-20)
            if np.dot(n,look)>0:continue
            color=np.array([239,101,72]) if face['group']=='posts' else np.array([174,174,169])
            light=.48+.48*abs(np.dot(n, np.array([-.3,.8,-.5])))
            color=tuple((color*light).clip(0,255).astype(int))
            points=np.stack([t@right,t@up],axis=1)*10500
            points[:,0]+=650;points[:,1]=450-points[:,1]
            triangles.append((float(t.mean(axis=0)@look),points,color))
im=Image.new('RGB',(1300,1050),'#f2f1ec');draw=ImageDraw.Draw(im)
for depth,pts,color in sorted(triangles,key=lambda x:x[0],reverse=True):draw.polygon([tuple(v) for v in pts],fill=color)
im.save(root/'tools/cad-output/posts-diagnostic.png')
