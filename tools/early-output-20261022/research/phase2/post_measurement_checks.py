from pathlib import Path
import subprocess,json,os,re,collections
p=Path(__file__).resolve().parents[2];d=p/'out/phase2/post-checks';d.mkdir(exist_ok=True);rows=[]
# Do not run concurrently with paced measurement.
assert json.loads((p/'out/phase2/current-stage.json').read_text())['stage']=='complete'
for name in ['wrap','odd-poc','fixed0','missing-timing','mmco5','bad-cpb-repeat','negative-leading']:
 file=p/f'out/phase2/hrd-boundaries/{name}.h264'
 for mode,t in [('legacy',1),('legacy',2),('legacy',12),('async',2),('async',12)]:
  pair=[]
  for on in [0,1]:
   pre=d/f'{name}-{mode}-t{t}-on{on}';env=dict(os.environ,EARLY_OUTPUT=str(on),SYNTHETIC_PTS='1');env.pop('H264_CERTIFICATE',None)
   if name=='negative-leading':env['TEST_FIRST_TWO_REORDER']='1'
   cmd=[str(p/'out/install/phase2/bin/probe'),str(file),str(t),'0',mode,str(pre),'3','0','1']
   with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=45)
   pair.append(dict(command=cmd,returncode=r.returncode,prefix=str(pre),frames=[l.split(',') for l in pre.with_suffix('.sha256').read_text().splitlines()],terminal=re.findall(r'terminal_return=(-?\d+)',pre.with_suffix('.log').read_text())))
  a=pair[0].pop('frames');b=pair[1].pop('frames');rows.append(dict(name=name,mode=mode,threads=t,off=pair[0],on=pair[1],counts=[len(a),len(b)],metadata_except_dts_equal=len(a)==len(b) and all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(a,b)),dts_changes=sum(x[2]!=y[2] for x,y in zip(a,b))))
(d/'boundaries.json').write_text(json.dumps(rows,indent=2)+'\n')
# Preserve and distinguish the existing nondeterministic error-flag race in lenient truncation.
errors=[]
for build in ['baseline','phase2']:
 for rep in range(5):
  for strict in [0,1]:
   pre=d/f'truncated-{build}-r{rep}-strict{strict}';env=dict(os.environ,SYNTHETIC_PTS='1')
   if strict:env['STRICT_ERRORS']='1'
   if build=='phase2':env['EARLY_OUTPUT']='0'
   cmd=[str(p/f'out/install/{build}/bin'/('phase2_probe' if build=='baseline' else 'probe')),str(p/'out/samples/poc2-first-idr-truncated.h264'),'2','0','async',str(pre),'1','0','1']
   with pre.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=30)
   frames=[l.split(',') for l in pre.with_suffix('.sha256').read_text().splitlines()];log=pre.with_suffix('.log').read_text();errors.append(dict(build=build,repeat=rep,strict=strict,returncode=r.returncode,terminal=re.findall(r'terminal_return=(-?\d+)',log),frames=len(frames),error_flags=[x[16] for x in frames],prefix=str(pre)))
(d/'error-return-ledger.json').write_text(json.dumps(errors,indent=2)+'\n')
# Actual NAL/slice structure and in-band proof flags: all HEVC and HRD H264 targets.
headers=[]
for folder in ['targets','hrd-targets']:
 for entry in json.loads((p/f'out/phase2/{folder}/manifest.json').read_text()):
  if folder=='targets' and entry['codec']!='x265':continue
  file=Path(entry['file']);pre=d/f'{folder}-{file.stem}.headers.log';cmd=['/opt/homebrew/bin/ffmpeg','-v','verbose','-i',str(file),'-c','copy','-bsf:v','trace_headers','-f','null','-']
  with pre.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=45)
  text=pre.read_text();nal=None;refb=0
  for line in text.splitlines():
   m=re.search(r'\b(nal_unit_type|slice_type)\s+[01]+\s+=\s+(\d+)',line)
   if m:
    if m[1]=='nal_unit_type':nal=int(m[2])
    elif entry['codec']=='x265' and int(m[2])==0 and nal in [1,3,5,7,9]:refb+=1
  fields={k:dict(collections.Counter(re.findall(r'\b'+k+r'\s+[01]+\s+=\s+(\d+)',text))) for k in ['fixed_frame_rate_flag','nal_hrd_parameters_present_flag','low_delay_hrd_flag','pic_struct_present_flag','cpb_cnt_minus1','cpb_removal_delay_length_minus1','dpb_output_delay_length_minus1','nal_unit_type']}
  headers.append(dict(file=file.name,folder=folder,command=cmd,returncode=r.returncode,fields=fields,hevc_reference_B_slices=refb))
(d/'actual-header-structures.json').write_text(json.dumps(headers,indent=2)+'\n')
print('post boundary pairs',len(rows),'pass',sum(x['metadata_except_dts_equal'] for x in rows),flush=True)
