from pathlib import Path
import subprocess,os,json,re
p=Path(__file__).resolve().parents[2];d=p/'out/phase3'/os.getenv('REPRESENTATIVE_DIR','representatives');d.mkdir(exist_ok=True);rows=[]
names=['fixed3-pyr1-open0','fixed7-pyr1-open1','bdominant16-pyr1-open1','adaptive16-pyr1-open1']
files=[p/f'out/phase2/hrd-targets/x264-{x}.mp4' for x in names]+[p/'out/phase2/targets/x265-fixed7-pyr1-open1.mp4',p/'out/phase2/targets/x264-fixed7-pyr1-open1.mp4']
def run(file,build,mode,t,early,cycles=1,extra=None,tag=''):
 pre=d/f'{tag}{file.stem}-{build}-{mode}-t{t}-on{early}-c{cycles}'
 env=dict(os.environ);env.pop('H264_CERTIFICATE',None);env.pop('DYLD_INSERT_LIBRARIES',None);env.update(extra or {})
 if build=='phase3':env['EARLY_OUTPUT']=str(early)
 binary='probe' if build=='phase3' else 'phase2_probe'
 cmd=[str(p/f'out/install/{build}/bin/{binary}'),str(file),str(t),'0',mode,str(pre),str(cycles),'0','1']
 with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=40)
 return dict(command=cmd,env={k:env[k] for k in ['EARLY_OUTPUT','H264_CERTIFICATE'] if k in env},prefix=str(pre),ret=r.returncode,terminal=re.findall(r'terminal_return=(-?\d+)',pre.with_suffix('.log').read_text()),frames=[l.split(',') for l in pre.with_suffix('.sha256').read_text().splitlines()])
def compare(a,b,**extra):
 x=a.pop('frames');y=b.pop('frames');mult=len(y)//len(x) if x else 1;x=x*mult
 rows.append(dict(**extra,reference=a,result=b,counts=[len(x),len(y)],other_metadata_equal=len(x)==len(y) and all(u[:2]+u[3:]==v[:2]+v[3:] for u,v in zip(x,y)),differing_columns=sorted(set(k for u,v in zip(x,y) for k in range(len(u)) if u[k]!=v[k])),dts_changes=sum(u[2]!=v[2] for u,v in zip(x,y))))
 (d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(extra,rows[-1]['counts'],rows[-1]['other_metadata_equal'],rows[-1]['differing_columns'],flush=True)
for file in files:
 for mode,t in [('legacy',1),('legacy',2),('legacy',12),('async',2),('async',12)]:
  for early in [0,1]:
   a=run(file,'baseline','legacy',t,0)
   b=run(file,'phase3',mode,t,early,3)
   compare(a,b,file=file.name,mode=mode,threads=t,early=early)
# Offline proof mode remains separate and is not used above or for measurement.
file=p/'out/phase2/targets/x264-fixed3-pyr1-open0.mp4';cert=p/'out/phase2/target-proof-final/x264-fixed3-pyr1-open0.packet-sha256'
for mode,t in [('legacy',1),('async',2),('async',12)]:
 compare(run(file,'baseline','legacy',t,0,tag='cert-'),run(file,'phase3',mode,t,1,3,{'H264_CERTIFICATE':str(cert)},'cert-'),file=file.name,mode=mode,threads=t,certificate=True)
for name in ['camp_mot_mbaff0_full.26l','Sharp_MP_PAFF_1r2.jvt','extradata-reload-multi-stsd.mov','pcm-poc0-leading-negative.h264','pcm-poc2-invalid-leading.h264']:
 file=p/'out/samples'/name
 for early in [0,1]:compare(run(file,'baseline','async',2,0,tag='fallback-'),run(file,'phase3','async',2,early,tag='fallback-'),file=name,fallback=True,early=early)
for codec,t in [('x264',12),('x265',2)]:
 file=p/f'out/phase2/{"hrd-targets" if codec=="x264" else "targets"}/{codec}-adaptive16-pyr1-open1.mp4'
 for mode in ['gate','flush','error','close','api']:
  compare(run(file,'phase3',mode,t,0,3,tag='life-'),run(file,'phase3',mode,t,1,3,tag='life-'),file=file.name,lifecycle=mode,threads=t)
print('representative comparisons',len(rows),'failures',sum(not x['other_metadata_equal'] for x in rows),flush=True)
