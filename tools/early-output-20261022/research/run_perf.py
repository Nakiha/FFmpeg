from pathlib import Path
import subprocess,os,json,re,statistics
p=Path(__file__).resolve().parents[1];d=p/'out'/('perf-'+__import__('datetime').datetime.now().strftime('%Y%m%dT%H%M%S%f'));d.mkdir(exist_ok=False);rows=[]
for sample,fps,cycles,t,mode in [('pcm-poc2-declared-reorder.h264',25,1,1,'legacy'),('pcm-poc2-declared-reorder.h264',25,1,2,'async'),('pcm-poc2-declared-reorder.h264',0,100,1,'legacy'),('x264-b7-open1.mp4',60,1,2,'async'),('x264-b7-open1.mp4',0,3,1,'legacy')]:
 for repeat in range(5):
  for on in ([0,1] if repeat%2==0 else [1,0]):
   pref=d/f'{Path(sample).stem}-fps{fps}-t{t}-{mode}-r{repeat}-on{on}'
   cmd=[str(p/'out/install/candidate/bin/perf_probe'),str(p/'out/samples'/sample),str(t),str(fps),mode,str(pref),str(cycles),'0','1']
   env=dict(os.environ,H264_EARLY=str(on),SYNTHETIC_PTS='1',NO_HASH='1')
   with pref.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60)
   log=pref.with_suffix('.log').read_text();lines=pref.with_suffix('.csv').read_text().splitlines();frames=[x.split(',') for x in lines if x.startswith('frame,')];summary=next((x for x in lines if x.startswith('summary,')),'')
   def val(k):m=re.search(k+r'=(\d+)',log+' '+summary);return int(m.group(1)) if m else None
   n=len(frames);wall=val('decode_wall_us');cpu=val('api_caller_cpu_ns');lat=[int(x[4])/1000 for x in frames]
   row=dict(sample=sample,fps=fps,threads=t,mode=mode,cycles=cycles,repeat=repeat,on=on,returncode=r.returncode,frames=n,first_frame_ms=val('first_frame_us')/1000 if val('first_frame_us') is not None else None,latency_mean_ms=statistics.mean(lat) if lat else None,latency_p95_ms=sorted(lat)[int(.95*(n-1))] if n else None,throughput_fps=n*1e6/wall if wall else None,api_caller_cpu_ms_per_frame=cpu/n/1e6 if n and mode=='legacy' else None,probe_process_cpu_ms_per_frame=val('cpu_us')/n/1000 if n else None,max_outstanding_at_output=max(int(x[5]) for x in frames) if n else None,command=cmd,env={'H264_EARLY':str(on),'SYNTHETIC_PTS':'1','NO_HASH':'1'},prefix=str(pref))
   rows.append(row);(d/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(sample,fps,mode,repeat,on,row['first_frame_ms'],flush=True)
