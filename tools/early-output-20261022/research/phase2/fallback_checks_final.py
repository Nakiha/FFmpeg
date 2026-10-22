from pathlib import Path
import subprocess,os,json
p=Path(__file__).resolve().parents[2];d=p/'out/phase2/fallback-final';d.mkdir(exist_ok=True);rows=[]
files=['BA1_FT_C.264','Sharp_MP_PAFF_1r2.jvt','camp_mot_fld0_full.26l','camp_mot_mbaff0_full.26l','camp_mot_picaff0_full.26l','intra_refresh.h264','twofields_packet.mp4','extradata-reload-multi-stsd.mov','pcm-poc0-leading-negative.h264','pcm-poc0-mmco5.h264','pcm-poc1-reorder.h264','pcm-poc2-declared-reorder.h264','pcm-poc2-invalid-leading.h264','pcm-poc2-wrap.h264','poc2-first-idr-truncated.h264','poc2-truncated.h264','missing-pps.h264']
def run(file,build,mode,t,early):
 pre=d/f'{Path(file).stem}-{build}-{mode}-t{t}-on{early}';env=dict(os.environ,SYNTHETIC_PTS='1')
 env.pop('H264_CERTIFICATE',None)
 if build=='phase2':env['EARLY_OUTPUT']=str(early)
 cmd=[str(p/f'out/install/{build}/bin'/('probe' if build=='phase2' else 'portable_probe')),str(p/'out/samples'/file),str(t),'0',mode,str(pre),'1','0','1']
 try:
  with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=45)
  ret=r.returncode
 except subprocess.TimeoutExpired:ret='timeout'
 data=pre.with_suffix('.sha256').read_text().splitlines()
 return dict(cmd=cmd,prefix=str(pre),returncode=ret,frames=data)
for file in files:
 for mode,t in [('legacy',1),('legacy',2),('legacy',12),('async',2),('async',12)]:
  a=run(file,'baseline',mode,t,0);aa=a.pop('frames')
  for early in [0,1]:
   b=run(file,'phase2',mode,t,early);bb=b.pop('frames');row=dict(file=file,mode=mode,threads=t,early=early,baseline=a,result=b,counts=[len(aa),len(bb)],exact=aa==bb and a['returncode']==b['returncode'])
   if not row['exact']:row['first_differences']=[dict(index=i,baseline=x,result=y) for i,(x,y) in enumerate(zip(aa,bb)) if x!=y][:2]
   rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');print(file,mode,t,early,row['counts'],row['exact'],flush=True)
