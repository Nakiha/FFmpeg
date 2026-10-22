from pathlib import Path
import subprocess,os,json,hashlib,re
p=Path(__file__).resolve().parents[2];scope={'__file__':str(p/'research/phase3/generate_boundaries.py')}
src=(p/'research/phase3/generate_boundaries.py').read_text();exec(src[:src.index('for name,point')],scope)
Bits,nal,pps,picture,sei,sps,bp=(scope[k] for k in ['Bits','nal','pps','picture','sei','sps','bp'])
d=p/'out/phase3'/os.getenv('EXTRA_DIR','extra-boundaries');d.mkdir(exist_ok=True);fixtures=[];runs=[]
def changed_pps():
 b=Bits().ue(0).ue(0).u(0,1).u(0,1).ue(0).ue(0).ue(0).u(0,1).u(0,2).se(1).se(0).se(0).u(1,1).u(0,1).u(0,1)
 return nal(0x68,b.finish())
for name in ['pre-anchor-sps-aba','short-bp','truncated-bp','duplicate-bp','pps-content-switch']:
 data=sps()+pps()
 for i in range(40):
  data+=nal(9,Bits().u(0,3).finish())
  if i==0:data+=bp()
  if name=='pre-anchor-sps-aba' and i in [1,2]:data+=sps(clock=100 if i==1 else 50)+pps()
  if i==11:
   if name=='short-bp':data+=sei(0,b'\x80')
   if name=='truncated-bp':data+=nal(6,bytes([0,16,0x80]))
   if name=='duplicate-bp':data+=bp()+bp()
   if name=='pps-content-switch':data+=changed_pps()
  data+=sei(1,bytes([((2*i)%16<<4)|4]))+picture(0,i,2*i,32+(i%16)*13,i==0,True)
 file=d/(name+'.h264');file.write_bytes(data);fixtures.append(dict(file=str(file),sha256=hashlib.sha256(data).hexdigest()))
 for mode,t in [('legacy',1),('async',12)]:
  pair=[]
  for early in [0,1]:
   pre=d/f'{name}-{mode}-on{early}';cmd=[str(p/'out/install/phase3/bin/probe'),str(file),str(t),'0',mode,str(pre),'3','0','1'];env=dict(os.environ,EARLY_OUTPUT=str(early),SYNTHETIC_PTS='1')
   with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=30)
   pair.append(dict(command=cmd,ret=r.returncode,terminal=re.findall(r'terminal_return=(-?\d+)',pre.with_suffix('.log').read_text()),prefix=str(pre),frames=[l.split(',') for l in pre.with_suffix('.sha256').read_text().splitlines()]))
  a=pair[0].pop('frames');b=pair[1].pop('frames');runs.append(dict(name=name,mode=mode,off=pair[0],on=pair[1],counts=[len(a),len(b)],other_metadata_equal=len(a)==len(b) and all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(a,b)),dts_changes=sum(x[2]!=y[2] for x,y in zip(a,b))))
(d/'manifest.json').write_text(json.dumps(fixtures,indent=2)+'\n');(d/'summary.json').write_text(json.dumps(runs,indent=2)+'\n')
print([(x['name'],x['mode'],x['other_metadata_equal'],x['dts_changes']) for x in runs])
