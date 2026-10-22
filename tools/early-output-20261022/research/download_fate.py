from pathlib import Path
import urllib.request,json,hashlib
root=Path(__file__).resolve().parents[1]/'out'
paths=['h264-conformance/camp_mot_fld0_full.26l','h264-conformance/camp_mot_mbaff0_full.26l','h264-conformance/camp_mot_picaff0_full.26l','h264-conformance/Sharp_MP_PAFF_1r2.jvt','h264-conformance/BA1_FT_C.264','h264/intra_refresh.h264','h264/extradata-reload-multi-stsd.mov','h264/twofields_packet.mp4']
records=[]
for path in paths:
 url='https://fate-suite.ffmpeg.org/'+path; dest=root/'samples'/Path(path).name
 try:
  with urllib.request.urlopen(url,timeout=45) as r:data=r.read(8*1024*1024+1)
  if len(data)>8*1024*1024: raise ValueError('sample exceeds per-file limit')
  dest.write_bytes(data)
  records.append(dict(url=url,path=str(dest),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),status='downloaded'))
 except Exception as e:records.append(dict(url=url,status='failed',error=str(e)))
 (root/'evidence/fate-download.json').write_text(json.dumps(records,indent=2)+'\n')
 print(records[-1],flush=True)
