from pathlib import Path
import json,hashlib,subprocess
p=Path(__file__).resolve().parents[2];scope={'__file__':str(p/'research/phase2/generate_hrd_boundaries.py')}
src=(p/'research/phase2/generate_hrd_boundaries.py').read_text();exec(src[:src.index('rows=[]')],scope)
Bits,nal,pps,picture,sei=(scope[k] for k in ['Bits','nal','pps','picture','sei'])
d=p/'out/phase3/fixtures';d.mkdir(exist_ok=True);rows=[]
def sps(fixed=True,sps_id=0,clock=50):
 b=Bits().u(77,8).u(0,8).u(30,8).ue(sps_id).ue(0).ue(0).ue(0).ue(4).u(0,1).ue(0).ue(0).u(1,1).u(1,1).u(0,1).u(1,1)
 b.u(0,4).u(1,1).u(1,32).u(clock,32).u(int(fixed),1).u(1,1)
 b.ue(0).u(0,4).u(0,4).ue(9999).ue(99999).u(0,1).u(15,5).u(3,5).u(3,5).u(0,5)
 b.u(0,1).u(0,1).u(0,1).u(1,1).u(1,1).ue(0).ue(0).ue(16).ue(16).ue(2).ue(4)
 return nal(0x67,b.finish())
def bp(sps_id=0):return sei(0,Bits().ue(sps_id).u(9000,16).u(0,16).finish())
for name,point in [('nonidr-bp',11),('bp-near-wrap',7),('bp-after-wrap',9),('bp-wrong-sps',11),('sps-clock-switch',11),('sps-fixed0-switch',11),('repeat-params',11)]:
 data=sps()+pps();origin=0;frames=[]
 if name=='bp-wrong-sps':data+=sps(sps_id=1)
 for i in range(40):
  data+=nal(9,Bits().u(0,3).finish())
  if i==0:data+=bp()
  if i==point:
   if name.startswith('bp-') or name=='nonidr-bp':data+=bp(1 if name=='bp-wrong-sps' else 0)
   if name=='sps-clock-switch':data+=sps(clock=100)+pps()
   if name=='sps-fixed0-switch':data+=sps(fixed=False)+pps()
   if name=='repeat-params':data+=sps()+pps()
  counter=(2*i-origin)%16
  data+=sei(1,bytes([(counter<<4)|4]))+picture(0,i,2*i,32+(i%16)*13,i==0,True)
  frames.append(dict(au=i,removal_tick=2*i,cpb_counter=counter,bp=i==0 or (i==point and (name.startswith('bp-') or name=='nonidr-bp'))))
  if i==point and (name.startswith('bp-') or name=='nonidr-bp'):origin=2*i
 f=d/(name+'.h264');f.write_bytes(data);rows.append(dict(file=str(f),sha256=hashlib.sha256(data).hexdigest(),frames=frames))
# A real three-slice HRD picture for later-NAL failure, rather than faking a
# second frame by concatenating two first_mb=0 slices.
f=d/'multislice-hrd.mp4'
cmd=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=60','-frames:v','192','-c:v','libx264','-threads','2','-x264-params','keyint=64:min-keyint=64:scenecut=0:bframes=3:b-adapt=0:b-pyramid=2:open-gop=0:force-cfr=1:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=2400:slices=3','-an',str(f)]
subprocess.run(cmd,check=True);rows.append(dict(file=str(f),command=cmd,sha256=hashlib.sha256(f.read_bytes()).hexdigest()))
(d/'boundary-manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
print('generated',len(rows),'boundary files')
