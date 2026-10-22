from pathlib import Path
import subprocess,os,json,re
p=Path(__file__).resolve().parents[2];d=p/'out/phase2/lifecycle';d.mkdir(exist_ok=True);rows=[]
for file in [p/'out/phase2/hrd-targets/x264-adaptive16-pyr1-open1.mp4',p/'out/phase2/targets/x265-adaptive16-pyr1-open1.mp4']:
 for t in [2,12]:
  for mode in ['gate','flush','error','close','api']:
   pair=[]
   for early in [0,1]:
    pre=d/f'{file.stem}-t{t}-{mode}-early{early}';cmd=[str(p/'out/install/phase2/bin/probe'),str(file),str(t),'0',mode,str(pre),'3','0','1']
    env=dict(os.environ,EARLY_OUTPUT=str(early),FFMPEG_LIVE_AUTO_MIN='2');env.pop('H264_CERTIFICATE',None)
    with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=45)
    pair.append(dict(command=cmd,returncode=r.returncode,prefix=str(pre),frames=[l.split(',') for l in pre.with_suffix('.sha256').read_text().splitlines()],terminal=re.findall(r'terminal_return=(-?\d+)',pre.with_suffix('.log').read_text()),injected=re.findall(r'injected_error=(-?\d+)',pre.with_suffix('.log').read_text())))
   a=pair[0].pop('frames');b=pair[1].pop('frames');row=dict(file=file.name,threads=t,mode=mode,off=pair[0],on=pair[1],counts=[len(a),len(b)],same_pixels_pts_order=len(a)==len(b) and all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(a,b)));rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
# Exact certificate mismatch must be a pre-output fallback, not a guessed assertion.
file=p/'out/phase2/targets/x264-fixed7-pyr1-open1.mp4';wrong=p/'out/phase2/b3-contract.packet-sha256'
for early in [0,1]:
 pre=d/f'wrong-certificate-{early}';cmd=[str(p/'out/install/phase2/bin/probe'),str(file),'2','0','async',str(pre),'1','0','1']
 with pre.with_suffix('.log').open('w') as f:subprocess.run(cmd,env=dict(os.environ,EARLY_OUTPUT=str(early),H264_CERTIFICATE=str(wrong)),stdout=f,stderr=subprocess.STDOUT,check=True)
a=(d/'wrong-certificate-0.sha256').read_text();b=(d/'wrong-certificate-1.sha256').read_text();assert a==b
print('lifecycle pairs',len(rows),'match',sum(x['same_pixels_pts_order'] for x in rows),'certificate mismatch fallback exact',flush=True)
