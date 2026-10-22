#!/usr/bin/env python3
"""Verify an exact local H264 input's header-order contract; not a live-source guess.
No entropy/pixel decoding is used by this Python verifier. ffprobe is only a packet
extractor; its demux probing startup is counted separately, never hidden as latency.
The resulting certificate binds the extradata and ordered immutable packet bytes.
"""
from pathlib import Path
import subprocess,json,re,hashlib,struct,argparse,time
class Rejected(Exception):pass
class Bits:
 def __init__(self,data):self.d=bytes(data);self.i=0
 def u(self,n):
  if self.i+n>len(self.d)*8:raise Rejected('truncated header')
  v=0
  for _ in range(n):v=v*2+((self.d[self.i//8]>>(7-self.i%8))&1);self.i+=1
  return v
 def ue(self):
  n=0
  while self.u(1)==0:
   n+=1
   if n>30:raise Rejected('oversized Golomb')
  return (1<<n)-1+self.u(n)
 def se(self):v=self.ue();return (v+1)//2 if v&1 else -v//2
def rbsp(nal):return re.sub(b'\x00\x00\x03',b'\x00\x00',nal[1:])
def sps(nal):
 b=Bits(rbsp(nal));profile=b.u(8);b.u(16);sid=b.ue();chroma=1
 if profile in [100,110,122,244,44,83,86,118,128,138,139,134,135]:
  chroma=b.ue()
  if chroma==3 and b.u(1):raise Rejected('separate color planes')
  if b.ue()>2 or b.ue()>2:raise Rejected('unsupported bit depth')
  b.u(1)
  if b.u(1):
   for i in range(8 if chroma!=3 else 12):
    if b.u(1):
     last=nextscale=8
     for _ in range(16 if i<6 else 64):
      if nextscale:nextscale=(last+b.se()+256)%256
      last=nextscale or last
 fn=b.ue()+4;pt=b.ue()
 if pt!=0:raise Rejected('POC type is not 0')
 pb=b.ue()+4;b.ue();gaps=b.u(1);b.ue();b.ue();frameonly=b.u(1)
 if not frameonly:raise Rejected('field/PAFF/MBAFF SPS')
 if fn>16 or pb>16:raise Rejected('POC/frame_num width')
 return sid,dict(framebits=fn,pocbits=pb,chroma=chroma,gaps=gaps,raw=nal.hex())
def pps(nal):
 b=Bits(rbsp(nal));pid=b.ue();sid=b.ue();entropy=b.u(1);bottom=b.u(1)
 if bottom:raise Rejected('per-frame bottom POC offset')
 if b.ue()!=0:raise Rejected('slice groups/FMO')
 l0=b.ue()+1;l1=b.ue()+1;weighted=b.u(1);bip=b.u(2);b.se();b.se();b.se();b.u(1);b.u(1);redundant=b.u(1)
 return pid,dict(sid=sid,l0=l0,l1=l1,weighted=weighted,bip=bip,redundant=redundant,raw=nal.hex())
def picture(nal,ss,pp):
 typ=nal[0]&31;ref=(nal[0]>>5)&3;b=Bits(rbsp(nal));mb=b.ue();st=b.ue()%5;pid=b.ue()
 if st in [3,4]:raise Rejected('SP/SI slices')
 if pid not in pp:raise Rejected('missing PPS')
 p=pp[pid]
 if p['sid'] not in ss:raise Rejected('missing SPS')
 s=ss[p['sid']];fn=b.u(s['framebits'])
 if typ==5:b.ue()
 lsb=b.u(s['pocbits'])
 if p['redundant']:b.ue()
 if st==1:b.u(1)
 l0,l1=p['l0'],p['l1']
 if st in [0,1] and b.u(1):
  l0=b.ue()+1
  if st==1:l1=b.ue()+1
 if max(l0,l1)>32:raise Rejected('reference list overflow')
 if st!=2:
  for _ in range(2 if st==1 else 1):
   if b.u(1):
    for j in range(128):
     op=b.ue()
     if op==3:break
     if op not in [0,1,2]:raise Rejected('reference list opcode')
     b.ue()
    else:raise Rejected('unterminated reference list')
 if (p['weighted'] and st==0) or (p['bip']==1 and st==1):
  b.ue()
  if s['chroma']:b.ue()
  for count in ([l0,l1] if st==1 else [l0]):
   for _ in range(count):
    if b.u(1):b.se();b.se()
    if s['chroma'] and b.u(1):
     for _ in range(2):b.se();b.se()
 if ref:
  if typ==5:b.u(2)
  elif b.u(1):
   for _ in range(128):
    op=b.ue()
    if op==0:break
    if op==5:raise Rejected('MMCO5 reset')
    if op in [1,3]:b.ue()
    if op in [2,3,4,6]:b.ue()
    if op>6:raise Rejected('MMCO opcode')
   else:raise Rejected('unterminated MMCO')
 return dict(first_mb=mb,frame_num=fn,lsb=lsb,ref=ref,type=typ,kind=st,pps=pid,sps=p['sid'])
def unhex(text):
 return bytes.fromhex(''.join(line.split(':',1)[1].split('  ',1)[0].replace(' ','') for line in text.splitlines() if ':' in line))
def nals(data,nlength):
 if nlength:
  out=[];i=0
  while i<len(data):
   if i+nlength>len(data):raise Rejected('truncated AVCC length')
   n=int.from_bytes(data[i:i+nlength],'big');i+=nlength
   if not n or i+n>len(data):raise Rejected('invalid AVCC length')
   out.append(data[i:i+n]);i+=n
  return out
 return [n for n in re.split(b'\x00\x00\x00\x01|\x00\x00\x01',data) if n]
def verify(file):
 began=time.perf_counter();cmd=['/opt/homebrew/bin/ffprobe','-v','error','-select_streams','v:0','-show_streams','-show_packets','-show_data','-show_entries','stream=codec_name,extradata:packet=data,pts,dts,duration:packet_side_data=side_data_type','-of','json',str(file)]
 raw=subprocess.run(cmd,capture_output=True,text=True,check=True,timeout=90);info=json.loads(raw.stdout);stream=info['streams'][0]
 if stream['codec_name']!='h264':raise Rejected('not H264')
 extra=unhex(stream.get('extradata',''));packets=[unhex(x['data']) for x in info['packets']];ss={};pp={};nlength=0;seen=set();seqs=[];seq=[];lastref=None;pm=pl=0;current=None
 digest=hashlib.sha256();digest.update(struct.pack('>Q',len(extra)));digest.update(extra)
 def param(n):
  if n[0]&31==7:
   k,v=sps(n)
   if seq and k in ss and ss[k]['raw']!=v['raw']:raise Rejected('SPS change inside sequence')
   ss[k]=v
  elif n[0]&31==8:
   k,v=pps(n)
   if seq and k in pp and pp[k]['raw']!=v['raw']:raise Rejected('PPS change inside sequence')
   pp[k]=v
 if extra and extra[0]==1:
  nlength=(extra[4]&3)+1;i=6
  for _ in range(extra[5]&31):
   size=int.from_bytes(extra[i:i+2],'big');i+=2;param(extra[i:i+size]);i+=size
  count=extra[i];i+=1
  for _ in range(count):
   size=int.from_bytes(extra[i:i+2],'big');i+=2;param(extra[i:i+size]);i+=size
 else:
  for n in nals(extra,0):param(n)
 for index,(packet,meta) in enumerate(zip(packets,info['packets'])):
  digest.update(struct.pack('>Q',len(packet)));digest.update(packet)
  digest.update(struct.pack('>qqq',int(meta.get('pts',-9223372036854775808)),int(meta.get('dts',-9223372036854775808)),int(meta.get('duration',0))))
  if any(x.get('side_data_type')=='New Extradata' for x in meta.get('side_data_list',[])):raise Rejected('new extradata side data')
  if 'pts' not in meta:raise Rejected('missing picture PTS')
  if not nlength and meta['pts']==-9223372036854775808:raise Rejected('missing PTS')
  pics=[]
  for n in nals(packet,nlength):
   typ=n[0]&31
   if typ in [7,8]:param(n)
   if typ in [1,5]:pics.append(picture(n,ss,pp))
  if not pics:raise Rejected('AU has no picture')
  pic=pics[0]
  if pic['first_mb']!=0:raise Rejected('first slice missing')
  for other in pics[1:]:
   if other['first_mb']==0 or any(other[k]!=pic[k] for k in ['frame_num','lsb','ref','type','pps']):raise Rejected('mixed picture headers')
  s=ss[pic['sps']]
  if pic['type']==5:
   if seq:seqs.append(seq)
   seq=[];seen=set();lastref=None;pm=pl=0
   if pic['frame_num'] or pic['lsb']:raise Rejected('nonzero IDR origin')
  elif not seq:raise Rejected('sequence does not start with IDR')
  if lastref is not None and pic['frame_num'] != ((lastref+1)%(1<<s['framebits'])):raise Rejected('frame_num gap/reset')
  m=1<<s['pocbits'];msb=pm
  if pic['lsb']<pl and pl-pic['lsb']>=m//2:msb+=m
  elif pic['lsb']>pl and pic['lsb']-pl>m//2:msb-=m
  poc=msb+pic['lsb']
  if poc<0 or poc%2 or poc in seen:raise Rejected('negative/odd/duplicate presentation POC')
  seen.add(poc);seq.append(dict(packet=index,poc=poc,pts=int(meta['pts']),kind=pic['kind'],reference=bool(pic['ref'])))
  if pic['ref']:pm,pl,lastref=msb,pic['lsb'],pic['frame_num']
 if seq:seqs.append(seq)
 if not seqs:raise Rejected('no sequence')
 displayed=[x for seq in seqs for x in sorted(seq,key=lambda x:x['poc'])]
 if any(a['pts']>=b['pts'] for a,b in zip(displayed,displayed[1:])):raise Rejected('PTS is not strictly increasing in display order')
 return dict(status='verified',contract='progressive-even-poc0-v1',packet_stream_sha256=digest.hexdigest(),source_sha256=hashlib.sha256(Path(file).read_bytes()).hexdigest(),packets=len(packets),sequences=seqs,command=cmd,verification_wall_ms=(time.perf_counter()-began)*1000)
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('input',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
 try:r=verify(a.input)
 except Exception as e:r=dict(status='rejected',reason=str(e))
 a.output.write_text(json.dumps(r,indent=2)+'\n')
 if r['status']=='verified':a.output.with_suffix('.packet-sha256').write_text('progressive-even-poc0-v1:'+r['packet_stream_sha256']+'\n')
 print(r['status'],r.get('reason',''),flush=True)
