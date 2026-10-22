from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parents[2];scope={'__file__':str(p/'research/generate_pcm.py')};src=(p/'research/generate_pcm.py').read_text();exec(src[:src.index('OUT.mkdir')],scope);Bits,nal,pps,picture=(scope[k] for k in ['Bits','nal','pps','picture']);d=p/'out/phase2/hrd-boundaries';d.mkdir(exist_ok=True)
def sps(fixed=True):
 b=Bits().u(77,8).u(0,8).u(30,8).ue(0).ue(0).ue(0).ue(0).ue(4).u(0,1).ue(0).ue(0).u(1,1).u(1,1).u(0,1).u(1,1)
 b.u(0,4).u(1,1).u(1,32).u(50,32).u(int(fixed),1).u(1,1)
 b.ue(0).u(0,4).u(0,4).ue(9999).ue(99999).u(0,1).u(15,5).u(3,5).u(3,5).u(0,5)
 b.u(0,1).u(0,1).u(0,1).u(1,1).u(1,1).ue(0).ue(0).ue(16).ue(16).ue(2).ue(4)
 return nal(0x67,b.finish())
def sei(typ,payload):return nal(6,bytes([typ,len(payload)])+payload+b'\x80')
rows=[]
for mode in ['wrap','fixed0','missing-timing','mmco5','bad-cpb-repeat']:
 data=sps(mode!='fixed0')+pps();frames=[]
 for i in range(40):
  reset=mode=='mmco5' and i==12;fn=i if not(mode=='mmco5' and i>12) else i-12;poc=2*fn
  data+=nal(9,Bits().u(0,3).finish())
  if i==0:data+=sei(0,Bits().ue(0).u(9000,16).u(0,16).finish())
  if not(mode=='missing-timing' and i==12):
   cpb=(2*i)%16
   if mode=='bad-cpb-repeat' and i==12:cpb=(2*(i-1))%16
   data+=sei(1,bytes([(cpb<<4)|4]))
  data+=picture(0,fn,poc,32+(i%16)*13,i==0,True,reset)
  frames.append(dict(i=i,frame_num=fn%16,poc=poc,mmco5=reset))
 file=d/(mode+'.h264');file.write_bytes(data);rows.append(dict(file=str(file),sha256=hashlib.sha256(data).hexdigest(),frames=frames))
(d/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
data=sps()+pps()
for i,(fn,poc,ref,delay) in enumerate([(0,0,True,4),(1,-2,False,0),(1,2,True,2),(2,4,True,2),(3,6,True,2)]):
 data+=nal(9,Bits().u(0,3).finish())
 if i==0:data+=sei(0,Bits().ue(0).u(9000,16).u(0,16).finish())
 data+=sei(1,bytes([((2*i)%16<<4)|delay]))+picture(0,fn,poc,32+i*13,i==0,ref)
(d/'negative-leading.h264').write_bytes(data)
data=sps()+pps()
for i in range(40):
 data+=nal(9,Bits().u(0,3).finish())
 if i==0:data+=sei(0,Bits().ue(0).u(9000,16).u(0,16).finish())
 data+=sei(1,bytes([((2*i)%16<<4)|4]))+picture(0,i,i,32+(i%16)*13,i==0,True)
(d/'odd-poc.h264').write_bytes(data)
