from pathlib import Path
import subprocess,os,json
p=Path(__file__).resolve().parents[1];d=p/'out'/('last-regressions-'+__import__('datetime').datetime.now().strftime('%Y%m%dT%H%M%S%f'));d.mkdir(exist_ok=False);rows=[]
for name in ['pcm-poc0-forward-wrap.h264','poc2-first-idr-truncated.h264']:
 for mode,t in [('legacy',1),('legacy',2),('legacy',12),('async',2),('async',12)]:
  pair=[]
  for on in (0,1):
   pre=d/f'{Path(name).stem}-{mode}-t{t}-on{on}';cmd=[str(p/'out/install/candidate/bin/portable_probe'),str(p/'out/samples'/name),str(t),'0',mode,str(pre),'1','0','1']
   with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=dict(os.environ,H264_EARLY=str(on),SYNTHETIC_PTS='1',STRICT_ERRORS='1'),stdout=f,stderr=subprocess.STDOUT,timeout=30)
   pair.append(dict(command=cmd,returncode=r.returncode,frames=pre.with_suffix('.sha256').read_text().splitlines(),log=pre.with_suffix('.log').read_text()))
  rows.append(dict(input=name,mode=mode,threads=t,off=pair[0],on=pair[1],exact=pair[0]['returncode']==pair[1]['returncode'] and pair[0]['frames']==pair[1]['frames']))
(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
