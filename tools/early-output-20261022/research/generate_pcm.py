#!/usr/bin/env python3
"""Small deterministic 16x16 CAVLC I_PCM streams; no external encoder required."""
from pathlib import Path
import json,hashlib
OUT=Path(__file__).resolve().parents[1]/'out/samples'
class Bits:
 def __init__(s): s.b=''
 def u(s,v,n): s.b+=format(v,f'0{n}b'); return s
 def ue(s,v): t=bin(v+1)[2:];s.b+='0'*(len(t)-1)+t;return s
 def se(s,v):return s.ue(2*v-1 if v>0 else -2*v)
 def raw(s,v):s.b+=''.join(format(x,'08b') for x in v);return s
 def align(s):s.b+='0'*(-len(s.b)%8);return s
 def finish(s):s.b+='1';s.align();return bytes(int(s.b[i:i+8],2) for i in range(0,len(s.b),8))
def nal(header,rbsp):
 data=bytearray();zeros=0
 for x in rbsp:
  if zeros==2 and x<=3:data.append(3);zeros=0
  data.append(x);zeros=zeros+1 if x==0 else 0
 return b'\0\0\0\1'+bytes([header])+data
def sps(pt,reorder=2):
 b=Bits().u(77,8).u(0,8).u(30,8).ue(0).ue(0).ue(pt)
 if pt==0:b.ue(0)
 if pt==1:b.u(0,1).se(0).se(0).ue(1).se(2)
 b.ue(4).u(0,1).ue(0).ue(0).u(1,1).u(1,1).u(0,1).u(1,1)
 # VUI: aspect/overscan/video/chroma/timing/HRD flags absent, pic_struct absent
 b.u(0,8).u(1,1).u(1,1).ue(0).ue(0).ue(16).ue(16).ue(reorder).ue(4)
 return nal(0x67,b.finish())
def pps():
 b=Bits().ue(0).ue(0).u(0,1).u(0,1).ue(0).ue(0).ue(0).u(0,1).u(0,2).se(0).se(0).se(0).u(1,1).u(0,1).u(0,1)
 return nal(0x68,b.finish())
def picture(pt,fn,poc,level,idr=False,ref=True,mmco5=False):
 b=Bits().ue(0).ue(2).ue(0).u(fn%16,4)
 if idr:b.ue(0)
 if pt==0:b.u(poc%16,4)
 if pt==1:b.se(poc-2*fn)
 if ref:
  if idr:b.u(0,2)
  else:
   b.u(int(mmco5),1)
   if mmco5:b.ue(5).ue(0)
 b.se(0).ue(1).ue(25).align().raw(bytes([level])*256+bytes([128])*128)
 return nal((0x60 if ref else 0)+(5 if idr else 1),b.finish())
def make(name,pt,frames,reorder=2):
 data=sps(pt,reorder)+pps();manifest=[]
 for i,f in enumerate(frames):
  fn,poc,ref,*reset=f
  data+=nal(9,Bits().u(0,3).finish())
  data+=picture(pt,fn,poc,32+i*13,i==0,ref,bool(reset and reset[0]))
  manifest.append(dict(decode_index=i,frame_num=fn%16,poc_requested=poc,reference=ref,mmco5=bool(reset and reset[0]),luma=32+i*13))
 path=OUT/(name+'.h264');path.write_bytes(data)
 return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest(),poc_type=pt,reorder=reorder,frames=manifest)
OUT.mkdir(parents=True,exist_ok=True)
r=[]
r.append(make('pcm-poc0-leading-negative',0,[(0,0,True),(1,-2,False),(1,2,True),(2,4,True),(3,6,True)]))
r.append(make('pcm-poc0-gap',0,[(0,0,True),(1,4,True),(2,1,True),(3,2,True),(4,6,True)]))
r.append(make('pcm-poc1-reorder',1,[(0,0,True),(1,6,True),(2,2,True),(3,4,True),(4,8,True)]))
r.append(make('pcm-poc2-declared-reorder',2,[(i,2*i,True) for i in range(12)]))
r.append(make('pcm-poc2-no-reorder',2,[(i,2*i,True) for i in range(12)],0))
r.append(make('pcm-poc2-wrap',2,[(i,2*i,True) for i in range(17)]))
r.append(make('pcm-poc0-mmco5',0,[(0,0,True),(1,2,True),(2,4,True,True),(1,2,True),(2,4,True)]))
r.append(make('pcm-poc2-invalid-leading',2,[(0,0,True),(0,-1,False),(1,2,True),(2,4,True),(3,6,True)]))
r.append(make('pcm-poc0-forward-wrap',0,[(i,2*i,True) for i in range(12)]))
(OUT/'pcm-manifest.json').write_text(json.dumps(r,indent=2)+'\n')
