from pathlib import Path
import subprocess,json,hashlib
p=Path(__file__).resolve().parents[1]; out=p/'out/samples'; cmds=[]
for b in (0,3,7):
 for op in (0,1):
  dest=out/f'x264-b{b}-open{op}.mp4'
  cmd=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=60','-frames:v','192','-c:v','libx264','-threads','2','-x264-params',f'keyint=48:min-keyint=48:scenecut=0:bframes={b}:b-adapt=0:b-pyramid=2:open-gop={op}','-an',str(dest)]
  subprocess.run(cmd,check=True);cmds.append(dict(command=cmd,sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
for op in (0,1):
 dest=out/f'x265-b7-open{op}.mp4'
 cmd=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=60','-frames:v','192','-c:v','libx265','-threads','2','-x265-params',f'keyint=48:min-keyint=48:scenecut=0:bframes=7:b-adapt=0:b-pyramid=1:open-gop={op}:pools=2:frame-threads=2:log-level=error:hash=1','-tag:v','hvc1','-an',str(dest)]
 subprocess.run(cmd,check=True);cmds.append(dict(command=cmd,sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
(p/'out/evidence/generate-media.json').write_text(json.dumps(cmds,indent=2)+'\n')
