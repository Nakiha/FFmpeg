from pathlib import Path
import subprocess,os,json,re,statistics
p=Path(__file__).resolve().parents[2];d=p/'out/phase3/performance';d.mkdir(exist_ok=True);rows=[]
names=['fixed3-pyr1-open0','fixed7-pyr1-open1','bdominant16-pyr1-open1','adaptive16-pyr1-open1']
files=[p/f'out/phase2/hrd-targets/x264-{x}.mp4' for x in names]+[p/'out/phase2/targets/x265-fixed7-pyr1-open1.mp4']+[p/'out/phase2/targets/x264-fixed7-pyr1-open1.mp4']
def run(file,mode,t,early,repeat,fps,cycles,build='phase3'):
 prefix=d/f'{build}-{file.parent.name}-{file.stem}-{mode}-t{t}-early{early}-r{repeat}-fps{fps}'
 env=dict(os.environ,EARLY_OUTPUT=str(early),NO_HASH='1');env.pop('H264_CERTIFICATE',None);env.pop('SYNTHETIC_PTS',None)
 cmd=[str(p/f'out/install/{build}/bin/perf_probe'),str(file),str(t),str(fps),mode,str(prefix),str(cycles),'0','1']
 with prefix.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60)
 log=prefix.with_suffix('.log').read_text();csv=prefix.with_suffix('.csv').read_text().splitlines();frames=[x.split(',') for x in csv if x.startswith('frame,')];summary=next((x for x in csv if x.startswith('summary,')),'')
 def value(k):m=re.search(k+r'=(\d+)',log+' '+summary);return int(m[1]) if m else None
 latency=[int(x[4])/1000 for x in frames];planned=[int(x[6])/1000 for x in frames];n=len(frames);wall=value('decode_wall_us')
 row=dict(build=build,file=file.name,source_folder=file.parent.name,mode=mode,threads=t,early=early,repeat=repeat,fps=fps,cycles=cycles,returncode=r.returncode,terminal=re.findall(r'terminal_return=(-?\d+)',log),frames=n,first_frame_ms=value('first_frame_us')/1000,submit_latency_mean_ms=statistics.mean(latency),submit_latency_p95_ms=sorted(latency)[int(.95*(n-1))],planned_latency_mean_ms=statistics.mean(planned) if fps else None,planned_latency_p95_ms=sorted(planned)[int(.95*(n-1))] if fps else None,throughput_fps=n*1e6/wall,codec_api_cpu_ms_per_frame=value('api_caller_cpu_ns')/n/1e6 if t==1 else None,process_cpu_ms_per_frame=value('cpu_us')/n/1000,max_outstanding_at_output=max(int(x[5]) for x in frames),command=cmd,environment={'EARLY_OUTPUT':str(early),'NO_HASH':'1'},prefix=str(prefix))
 rows.append(row);(d/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(file.name,file.parent.name,mode,early,repeat,fps,round(row['submit_latency_mean_ms'],3),flush=True)
for file in files:
 for repeat in range(3):
  modes=['legacy','async'] if repeat%2==0 else ['async','legacy']
  for mode in modes:
   for early in ([0,1] if repeat%2==0 else [1,0]):run(file,mode,2,early,repeat,60,1)
# Independent no-hash single-thread CPU / uncapped throughput envelope.
for file in files:
 for repeat in range(3):
  for early in ([0,1] if repeat%2==0 else [1,0]):run(file,'legacy',1,early,repeat,0,5)
# Isolate the real finish_setup deferral cost at fixed2 frame threads, using the
# exact same H264 inputs and alternating old/new binaries, without paced input.
for file in files[:4]:
 for repeat in range(3):
  for build in (['phase2','phase3'] if repeat%2==0 else ['phase3','phase2']):
   for early in ([0,1] if repeat%2==0 else [1,0]):run(file,'legacy',2,early,repeat,0,5,build)
