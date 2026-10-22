from pathlib import Path
import subprocess,json,re,hashlib
p=Path(__file__).resolve().parents[1];d=p/'out'/('source-audit-'+__import__('datetime').datetime.now().strftime('%Y%m%dT%H%M%S%f'));d.mkdir(exist_ok=False);rows=[]
files=sorted(x for x in (p/'out/samples').iterdir() if x.suffix in ['.mp4','.mov','.h264','.26l','.264','.jvt'])
for file in files:
 trace=d/(file.name+'.headers.log');cmd=['/opt/homebrew/bin/ffmpeg','-v','verbose','-i',str(file),'-c','copy','-bsf:v','trace_headers','-f','null','-']
 with trace.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=45)
 text=trace.read_text(); fields={}
 for k in ['pic_order_cnt_type','frame_mbs_only_flag','mb_adaptive_frame_field_flag','field_pic_flag','bottom_field_flag','max_num_reorder_frames','memory_management_control_operation','recovery_frame_cnt','nal_unit_type']:
  fields[k]=dict(__import__('collections').Counter(re.findall(r'\b'+k+r'\s+[01]+\s+=\s+(-?\d+)',text)))
 ref=nal=None;refb=noni=0
 for line in text.splitlines():
  m=re.search(r'\b(nal_ref_idc|nal_unit_type|slice_type)\s+[01]+\s+=\s+(\d+)',line)
  if m:
   key,v=m[1],int(m[2])
   if key=='nal_ref_idc':ref=v
   if key=='nal_unit_type':nal=v
   if key=='slice_type' and nal==1:
    if v%5==1 and ref:refb+=1
    if v%5==2:noni+=1
 probe=['/opt/homebrew/bin/ffprobe','-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=pict_type,key_frame,interlaced_frame,top_field_first,repeat_pict','-of','json',str(file)]
 fp=subprocess.run(probe,capture_output=True,text=True,timeout=45);meta=json.loads(fp.stdout or '{}');(d/(file.name+'.frames.json')).write_text(json.dumps(meta,indent=2)+'\n')
 frames=meta.get('frames',[]);bmax=run=0
 for f in frames:
  run=run+1 if f.get('pict_type')=='B' else 0;bmax=max(run,bmax)
 row=dict(file=file.name,sha256=hashlib.sha256(file.read_bytes()).hexdigest(),bytes=file.stat().st_size,trace_return=r.returncode,headers=fields,reference_B_slices=refb,non_IDR_I_slices=noni,independent_frames=len(frames),maximum_consecutive_B=bmax,interlaced_frames=sum(f.get('interlaced_frame',0) for f in frames),top_first_frames=sum(f.get('top_field_first',0) for f in frames),commands=[cmd,probe])
 # Ordered pixel oracle is independent Homebrew 8.1.2, no acceptance claim on malformed streams.
 hashfile=d/(file.name+'.framehash');cmd2=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-i',str(file),'-map','0:v:0','-fps_mode','passthrough','-f','framehash',str(hashfile)]
 rr=subprocess.run(cmd2,capture_output=True,text=True,timeout=45);(d/(file.name+'.oracle.log')).write_text(rr.stderr)
 hashes=[line.split(',')[-1].strip() for line in hashfile.read_text().splitlines() if not line.startswith('#')] if hashfile.exists() else []
 baseline=p/'out/matrix'/f'{file.stem}-legacy-t1-baseline-on0-c1.sha256'
 if baseline.exists():
  expected=[line.split(',')[13] for line in baseline.read_text().splitlines()];row.update(oracle_return=rr.returncode,oracle_frames=len(hashes),ordered_pixels_match_independent=expected==hashes,oracle_limitation='CLI output encoder normalizes dimensions after SPS change' if file.name=='extradata-reload-multi-stsd.mov' else None)
 rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');print(file.name,fields.get('pic_order_cnt_type'),refb,noni,row.get('ordered_pixels_match_independent'),flush=True)
