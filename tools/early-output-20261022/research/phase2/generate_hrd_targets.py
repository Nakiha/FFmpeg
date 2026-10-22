from pathlib import Path
import subprocess,json,hashlib,itertools,time
p=Path(__file__).resolve().parents[2];d=p/'out/phase2/hrd-targets';d.mkdir(exist_ok=True);rows=[]
# Fixed 3B/7B and B-dominant 16B; dynamic adaptation has two maxima and content inversions.
profiles=[('fixed3',3,0),('fixed7',7,0),('bdominant16',16,0),('adaptive7',7,2),('adaptive16',16,2)]
for codec in ['x264']:
 for name,b,adapt in profiles:
  # Cover pyramid off/on and open/closed for fixed3/7; high-B/dynamic use pyramid on.
  for pyramid in ([0,1] if name in ['fixed3','fixed7'] else [1]):
   for op in [0,1]:
    dest=d/f'{codec}-{name}-pyr{pyramid}-open{op}.mp4'
    params=f'keyint=64:min-keyint=64:scenecut=0:bframes={b}:b-adapt={adapt}:b-pyramid={2 if codec=="x264" and pyramid else pyramid}:open-gop={op}'
    if codec=='x264':params+=':force-cfr=1:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=2400'
    if codec=='x265':params+=':pools=2:frame-threads=2:log-level=error:hash=1'
    cmd=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=60','-frames:v','192']
    if adapt:cmd+=['-vf',"negate=enable='lt(mod(n,37),9)'"]
    cmd+=['-c:v','lib'+codec,'-threads','2','-'+codec+'-params',params,'-an']
    if codec=='x265':cmd+=['-tag:v','hvc1']
    cmd+=[str(dest)]
    with dest.with_suffix('.encode.log').open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=120)
    rows.append(dict(file=str(dest),codec=codec,profile=name,b_max_request=b,b_adapt=adapt,pyramid=pyramid,open=op,command=cmd,sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
    (d/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(dest.name,flush=True)
