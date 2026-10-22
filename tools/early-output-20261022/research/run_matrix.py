from pathlib import Path
import subprocess,os,json,time,hashlib
p=Path(__file__).resolve().parents[1]; dst=p/'out'/('matrix-'+__import__('datetime').datetime.now().strftime('%Y%m%dT%H%M%S%f'));dst.mkdir(exist_ok=False); rows=[];commands=[]
files=sorted(x for x in (p/'out/samples').iterdir() if x.suffix in ['.mp4','.mov','.h264','.26l','.264','.jvt'])
def run(file,build,mode,t,on,cycles=1,extra=None,label=''):
 stem=f'{file.stem}-{mode}-t{t}-{build}-on{on}-c{cycles}{label}';pref=dst/stem
 env=dict(os.environ,SYNTHETIC_PTS='1');env.pop('H264_EARLY',None)
 if build=='candidate':env['H264_EARLY']=str(on)
 if extra:env.update(extra)
 cmd=[str(p/f'out/install/{build}/bin/portable_probe'),str(file),str(t),'0',mode,str(pref),str(cycles),'0','1'];commands.append(dict(cmd=cmd,env={k:env[k] for k in ('SYNTHETIC_PTS','H264_EARLY','FFMPEG_LIVE_IDR_EARLY','FFMPEG_LIVE_POC_EARLY') if k in env}))
 try:
  with pref.with_suffix('.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=45)
  code=r.returncode
 except subprocess.TimeoutExpired:code='timeout'
 digest=pref.with_suffix('.sha256'); data=[x.split(',') for x in digest.read_text().splitlines()] if digest.exists() else []
 return dict(returncode=code,frames=data,prefix=str(pref))
for file in files:
 for mode,t in [('legacy',1),('legacy',2),('legacy',12),('async',2),('async',12)]:
  a=run(file,'baseline',mode,t,0);b=run(file,'candidate',mode,t,0);c=run(file,'candidate',mode,t,1)
  aa=a.pop('frames');bb=b.pop('frames');cc=c.pop('frames')
  diff=[dict(index=i,off=x,on=y) for i,(x,y) in enumerate(zip(bb,cc)) if x!=y]
  cols=sorted({j for x,y in zip(bb,cc) for j in range(min(len(x),len(y))) if x[j]!=y[j]})
  row=dict(input=file.name,mode=mode,threads=t,baseline=a,candidate_off=b,candidate_on=c,counts=[len(aa),len(bb),len(cc)],baseline_vs_off=aa==bb and a['returncode']==b['returncode'],exact_off_on=bb==cc and b['returncode']==c['returncode'],differing_columns=cols,first_differences=diff[:2])
  rows.append(row);print(file.name,mode,t,row['counts'],row['baseline_vs_off'],row['exact_off_on'],cols,flush=True)
 (dst/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');(dst/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
