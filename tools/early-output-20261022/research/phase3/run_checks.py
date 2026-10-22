from pathlib import Path
import subprocess,os,json,re
p=Path(__file__).resolve().parents[2];d=p/'out/phase3'/os.getenv('CHECK_DIR','checks-final');d.mkdir(exist_ok=True);rows=[]
def run(file,mode,t,early,tag,extra=None,mutation=False,cycles=1):
 pre=d/tag;env=dict(os.environ,EARLY_OUTPUT=str(early));env.pop('H264_CERTIFICATE',None);env.pop('DYLD_INSERT_LIBRARIES',None)
 env.update(extra or {});bin=p/'out/phase3/mutation_probe-phase3' if mutation else p/'out/install/phase3/bin/probe'
 cmd=[str(bin),str(file),str(t),'0',mode,str(pre),str(cycles),'0','1']
 with pre.with_suffix('.log').open('w') as f:
  try:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=30);ret=r.returncode
  except subprocess.TimeoutExpired:ret='timeout'
 a=[x.split(',') for x in pre.with_suffix('.sha256').read_text().splitlines()];log=pre.with_suffix('.log').read_text()
 return dict(command=cmd,env={k:env[k] for k in ['EARLY_OUTPUT','AU_MUTATION','MUTATION_AU','STRICT_ERRORS','SYNTHETIC_PTS','FAIL_FIFO_WRITE','DYLD_INSERT_LIBRARIES'] if k in env},prefix=str(pre),ret=ret,terminal=re.findall(r'terminal_return=(-?\d+)',log),frames=a,log=log)
def compare(a,b,**attrs):
 x=a.pop('frames');y=b.pop('frames');a.pop('log');log=b.pop('log')
 row=dict(**attrs,off=a,on=b,counts=[len(x),len(y)],other_metadata_equal=len(x)==len(y) and all(u[:2]+u[3:]==v[:2]+v[3:] for u,v in zip(x,y)),differing_columns=sorted(set(k for u,v in zip(x,y) for k in range(len(u)) if u[k]!=v[k])),dts_changes=sum(u[2]!=v[2] for u,v in zip(x,y)))
 if 'mutation' in attrs:
  # Current bad packet12 DTS is2560 in the60fps MP4 targets. Detect actual
  # extension on that packet, not just eventual equality of the hash set.
  row['bad_au_extra_delivery_effect']=sum(v[2]=='2560' and u[2]!=v[2] for u,v in zip(x,y))
 row['fault_injected']='INJECTED_FIFO_ENOMEM' in log
 row['common_prefix_metadata_equal']=all(u[:2]+u[3:]==v[:2]+v[3:] for u,v in zip(x,y))
 row['terminal_status_equal']=a['ret']==b['ret'] and a['terminal']==b['terminal']
 row['extra_on_source_aus']=[v[18] for v in y[len(x):]]
 rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(attrs,len(x),len(y),row['other_metadata_equal'],row['differing_columns'],flush=True)
for kind in ['parser-error','truncated','short','duplicate','contradictory','late']:
 file=p/'out/phase2/hrd-targets/x264-fixed3-pyr1-open0.mp4'
 for mode,t in [('legacy',1),('async',2),('async',12)]:
  for strict in [0,1]:
   extra={'AU_MUTATION':kind};extra.update({'STRICT_ERRORS':'1'} if strict else {})
   a,b=[run(file,mode,t,on,f'{kind}-{mode}-t{t}-strict{strict}-on{on}',extra,True) for on in [0,1]]
   compare(a,b,mutation=kind,mode=mode,threads=t,strict=strict)
file=p/'out/phase3/fixtures/multislice-hrd.mp4'
for kind in ['none','late','parser-error','short','slice-error']:
 for mode,t in [('legacy',1),('async',2),('async',12)]:
  extra={} if kind=='none' else {'AU_MUTATION':kind}
  a,b=[run(file,mode,t,on,f'multislice-{kind}-{mode}-t{t}-on{on}',extra,kind!='none') for on in [0,1]]
  compare(a,b,mutation='multislice-'+kind,mode=mode,threads=t)
for name in ['nonidr-bp','bp-near-wrap','bp-after-wrap','bp-wrong-sps','sps-clock-switch','sps-fixed0-switch','repeat-params']:
 file=p/f'out/phase3/fixtures/{name}.h264'
 for mode,t in [('legacy',1),('legacy',2),('async',2),('async',12)]:
  a,b=[run(file,mode,t,on,f'{name}-{mode}-t{t}-on{on}',{'SYNTHETIC_PTS':'1'},cycles=3) for on in [0,1]]
  compare(a,b,boundary=name,mode=mode,threads=t)
file=p/'out/phase2/hrd-targets/x264-fixed3-pyr1-open0.mp4'
for mode,t in [('legacy',1),('async',2),('async',12)]:
 for failure in [1,2,3]:
  a=run(file,mode,t,0,f'oom-reference-{mode}-t{t}-n{failure}')
  b=run(file,mode,t,1,f'oom-injected-{mode}-t{t}-n{failure}',{'DYLD_INSERT_LIBRARIES':str(p/'out/phase3/fifo_failure_interpose.dylib'),'FAIL_FIFO_WRITE':str(failure)})
  compare(a,b,oom_fifo_write=failure,mode=mode,threads=t)
print('completed pairs',len(rows),'mismatches',sum(not x['other_metadata_equal'] for x in rows),flush=True)
