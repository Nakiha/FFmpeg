from pathlib import Path
import subprocess,re,json,hashlib
p=Path(__file__).resolve().parents[1];d=p/'out/samples';r=[]
for op in (0,1):
 source=d/f'x265-b7-open{op}.mp4';dest=d/f'x265-b7-open{op}.hevc'
 cmd=['/opt/homebrew/bin/ffmpeg','-v','error','-y','-i',str(source),'-c','copy','-bsf:v','hevc_mp4toannexb','-f','hevc',str(dest)];subprocess.run(cmd,check=True)
 data=dest.read_bytes();nals=[x for x in re.split(b'\x00\x00\x00\x01|\x00\x00\x01',data) if x];types=[(x[0]>>1)&63 for x in nals]
 r.append(dict(source=str(source),command=cmd,nal_types={str(t):types.count(t) for t in set(types)},sha256=hashlib.sha256(data).hexdigest()))
 if op==0:
  changed=[bytes([(x[0]&0x81)|(19<<1)])+x[1:] if ((x[0]>>1)&63)==20 else x for x in nals]
  (d/'hevc-idr-w-radl-no-leading.hevc').write_bytes(b''.join(b'\0\0\0\1'+x for x in changed))
  gap=[x for x in nals if ((x[0]>>1)&63)!=0]
  (d/'hevc-poc-gap.hevc').write_bytes(b''.join(b'\0\0\0\1'+x for x in gap))
# Deliberately malformed H264 fixtures retain exact input bytes for reproducibility.
data=(d/'pcm-poc2-declared-reorder.h264').read_bytes()
(d/'poc2-truncated.h264').write_bytes(data[:-210])
# A late malformed SEI in the first AU (before second AUD), exercised with EXPLODE.
starts=list(re.finditer(b'\x00\x00\x00\x01\x09',data));pos=starts[1].start()
(d/'poc2-late-bad-sei.h264').write_bytes(data[:pos]+b'\0\0\0\1\x06\x05\xff\x80'+data[pos:])
(d/'missing-pps.h264').write_bytes(re.sub(b'\x00\x00\x00\x01\x68.*?(?=\x00\x00\x00\x01)',b'',data,flags=re.S,count=1))
(p/'out/evidence/boundary-preparation.json').write_text(json.dumps(r,indent=2)+'\n')
