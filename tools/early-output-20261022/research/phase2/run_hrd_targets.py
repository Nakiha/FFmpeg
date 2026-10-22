from pathlib import Path
import os,json,subprocess,time,statistics,collections,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from verify_contract import verify
p=Path(__file__).resolve().parents[2];d=p/'out/phase2/hrd-checks';d.mkdir(exist_ok=True);rows=[];certs=[];structure=[]
for entry in json.loads((p/'out/phase2/hrd-targets/manifest.json').read_text()):
 file=Path(entry['file']);certificate=None
 if entry['codec']=='x264':
  try:result=verify(file)
  except Exception as e:result=dict(status='rejected',reason=str(e))
  result['file']=str(file);certs.append(result);(d/'certificates.json').write_text(json.dumps(certs,indent=2)+'\n')
  if result['status']=='verified':
   certificate=d/(file.stem+'.packet-sha256');certificate.write_text('progressive-even-poc0-v1:'+result['packet_stream_sha256']+'\n')
   seq=[x for part in result['sequences'] for x in sorted(part,key=lambda x:x['poc'])]
   kinds=[x['kind'] for x in seq];runs=[];run=0
   for k in kinds+[None]:
    if k==1:run+=1
    elif run:runs.append(run);run=0
   structure.append(dict(file=file.name,frames=len(seq),B=sum(k==1 for k in kinds),P=sum(k==0 for k in kinds),I=sum(k==2 for k in kinds),reference_B=sum(x['kind']==1 and x['reference'] for x in seq),B_run_histogram=dict(collections.Counter(runs)),max_B_run=max(runs,default=0)))
 else:
  raw=subprocess.run(['/opt/homebrew/bin/ffprobe','-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=pict_type','-of','json',str(file)],capture_output=True,text=True,check=True)
  kinds=[x.get('pict_type') for x in json.loads(raw.stdout)['frames']];runs=[];run=0
  for k in kinds+[None]:
   if k=='B':run+=1
   elif run:runs.append(run);run=0
  structure.append(dict(file=file.name,frames=len(kinds),B=kinds.count('B'),P=kinds.count('P'),I=kinds.count('I'),B_run_histogram=dict(collections.Counter(runs)),max_B_run=max(runs,default=0)))
 (d/'actual-structures.json').write_text(json.dumps(structure,indent=2)+'\n')
 certificate=None # Automatic in-band HRD mode: no certificate is ever supplied.
 def run(build,mode,t,early,cycles=1):
  prefix=d/f'{file.stem}-{build}-{mode}-t{t}-early{early}-c{cycles}';env=dict(os.environ)
  if build=='phase2':
   env['EARLY_OUTPUT']=str(early)
   if certificate:env['H264_CERTIFICATE']=str(certificate)
  cmd=[str(p/f'out/install/{build}/bin'/('probe' if build=='phase2' else 'portable_probe')),str(file),str(t),'0',mode,str(prefix),str(cycles),'0','1']
  try:
   with prefix.with_suffix('.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60)
   ret=r.returncode
  except subprocess.TimeoutExpired:ret='timeout'
  data=[s.split(',') for s in prefix.with_suffix('.sha256').read_text().splitlines()]
  return dict(ret=ret,frames=data,prefix=str(prefix),command=cmd,env={k:env[k] for k in ['EARLY_OUTPUT','H264_CERTIFICATE'] if k in env})
 for t in [1,2,12]:
  base=run('baseline','legacy',t,0);a=base.pop('frames')
  for mode in (['legacy'] if t==1 else ['legacy','async']):
   for early in [0,1]:
    x=run('phase2',mode,t,early,3);b=x.pop('frames');ref=a*3
    equal=len(ref)==len(b) and all(i[:2]+i[3:]==j[:2]+j[3:] for i,j in zip(ref,b))
    cols=sorted({z for i,j in zip(ref,b) for z in range(len(i)) if i[z]!=j[z]})
    row=dict(file=file.name,codec=entry['codec'],threads=t,mode=mode,early=early,baseline=base,result=x,counts=[len(ref),len(b)],pixels_order_pts_metadata_equal=equal,exact=ref==b,differing_columns=cols)
    rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');print(file.name,t,mode,early,row['counts'],equal,cols,flush=True)
