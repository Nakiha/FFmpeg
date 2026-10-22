from pathlib import Path
import subprocess,os,json,itertools
p=Path(__file__).resolve().parents[1];d=p/'out'/('boundaries-'+__import__('datetime').datetime.now().strftime('%Y%m%dT%H%M%S%f'));d.mkdir(exist_ok=False);rows=[]
def run(file,t,mode,early,cycles=1,hevc=False,extra=None):
 file=p/'out/samples'/file
 label='-'.join(f'{k}={v}' for k,v in (extra or {}).items())
 pref=d/f'{file.stem}-t{t}-{mode}-on{early}-c{cycles}-{label}'
 env=dict(os.environ)
 for k in ['H264_EARLY','FFMPEG_LIVE_IDR_EARLY','FFMPEG_LIVE_POC_EARLY','SYNTHETIC_PTS']:env.pop(k,None)
 if hevc:
  if early:env.update(FFMPEG_LIVE_IDR_EARLY='1',FFMPEG_LIVE_POC_EARLY='1')
 else:env['H264_EARLY']=str(early)
 if extra:env.update(extra)
 cmd=[str(p/'out/install/candidate/bin/portable_probe'),str(file),str(t),'0',mode,str(pref),str(cycles),'0','1']
 try:
  with pref.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=40)
  code=r.returncode
 except subprocess.TimeoutExpired:code='timeout'
 sha=pref.with_suffix('.sha256');data=[x.split(',') for x in sha.read_text().splitlines()] if sha.exists() else []
 return dict(returncode=code,frames=data,prefix=str(pref),cmd=cmd,env={k:env[k] for k in ['H264_EARLY','FFMPEG_LIVE_IDR_EARLY','FFMPEG_LIVE_POC_EARLY','SYNTHETIC_PTS','STRICT_ERRORS','SEEK_US','FFMPEG_LIVE_AUTO_MIN'] if k in env})
def pair(file,t,mode,cycles=1,hevc=False,extra=None):
 a=run(file,t,mode,0,cycles,hevc,extra);b=run(file,t,mode,1,cycles,hevc,extra);aa=a.pop('frames');bb=b.pop('frames')
 cols=sorted({j for x,y in zip(aa,bb) for j in range(min(len(x),len(y))) if x[j]!=y[j]})
 row=dict(file=file,threads=t,mode=mode,cycles=cycles,hevc=hevc,extra=extra,off=a,on=b,counts=[len(aa),len(bb)],exact=aa==bb and a['returncode']==b['returncode'],pixel_presentation_metadata_except_trigger_dts=len(aa)==len(bb) and all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(aa,bb)),differing_columns=cols,first_difference=next((dict(index=i,off=x,on=y) for i,(x,y) in enumerate(zip(aa,bb)) if x!=y),None))
 rows.append(row);(d/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');print(file,t,mode,cycles,row['counts'],a['returncode'],b['returncode'],cols,flush=True)
# Native PTS for all timestamped generated streams and FATE containers.
for file in [*(f'x264-b{b}-open{o}.mp4' for b,o in itertools.product((0,3,7),(0,1))),'extradata-reload-multi-stsd.mov','twofields_packet.mp4']:
 for t in (1,2,12):pair(file,t,'legacy')
 for t in (2,12):pair(file,t,'async',3)
# Flush/replay, in-flight flush, closed gate, bad-packet recovery, close/API.
for file in ['pcm-poc2-declared-reorder.h264','x264-b7-open1.mp4']:
 for t in (2,12):
  for mode in ('async','gate','flush','error','close','api'):pair(file,t,mode,3,extra={'SYNTHETIC_PTS':'1','FFMPEG_LIVE_AUTO_MIN':'2'})
# Malformed streams; preserve exact return code and ordered prefix even if error.
for file in ['poc2-truncated.h264','poc2-late-bad-sei.h264','missing-pps.h264']:
 for t in (1,2,12):pair(file,t,'legacy',extra={'SYNTHETIC_PTS':'1','STRICT_ERRORS':'1'})
 for t in (2,12):pair(file,t,'async',extra={'SYNTHETIC_PTS':'1','STRICT_ERRORS':'1'})
# Real demux seek then flush/replay on generated open GOP, native PTS.
for t in (1,2,12):pair('x264-b7-open1.mp4',t,'legacy',3,extra={'SEEK_US':'1200000'})
# HEVC existing policy, including raw missing PTS, changed IDR type and missing nonrefs.
for file in ['x265-b7-open0.mp4','x265-b7-open1.mp4','x265-b7-open0.hevc','x265-b7-open1.hevc','hevc-idr-w-radl-no-leading.hevc','hevc-poc-gap.hevc']:
 for t in (2,12):
  pair(file,t,'async',3,hevc=True)
  if file.endswith('.hevc'):pair(file,t,'async',3,hevc=True,extra={'SYNTHETIC_PTS':'1'})
for mode in ('gate','flush','error','close','api','checksum'):
 pair('x265-b7-open0.mp4',2,mode,hevc=True)
