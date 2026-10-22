#!/usr/bin/env python3
"""Continuous replay correctness + separate CPU/throughput workload. Linux only.
No hashing in perf timing. All demux/allocation and output I/O are outside it.
The full declared replay count, identities and observations must validate.
"""
import argparse
import csv
import itertools
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
from evidence import require, sha256
from run_representatives import existing_input, verify_identity


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--workspace',type=Path,required=True)
    ap.add_argument('--identity',type=Path,required=True)
    ap.add_argument('--results',type=Path,required=True)
    ap.add_argument('--replays',type=int,default=30)
    ap.add_argument('--repeats',type=int,default=3)
    ap.add_argument('--additional-manifest',type=Path,help='Optional independently declared [{path,frames,sha256}] fixtures')
    args=ap.parse_args()
    require(args.replays>=3 and args.repeats>=3,'at least three repeats and replays required')
    builds=json.loads(args.identity.read_text())['builds']
    args.results.mkdir(parents=True,exist_ok=False)
    fixtures=[('hrd-targets','x264-'+n+'.mp4') for n in ['fixed3-pyr1-open0','fixed7-pyr1-open1','adaptive16-pyr1-open1']]
    fixtures += [('targets','x265-fixed7-pyr1-open1.mp4'),('targets','x264-fixed7-pyr1-open1.mp4')]
    inputs=[existing_input(args.workspace,folder,name) for folder,name in fixtures]
    if args.additional_manifest:
        for entry in json.loads(args.additional_manifest.read_text()):
            file=Path(entry['path']);require(sha256(file)==entry['sha256'] and entry['frames']>=96,'invalid additional fixture')
            inputs.append((file,entry['frames']))
    rows=[]
    def run(build_name,file,frames,mode,threads,early,workload,replays,tag):
        build=builds[build_name];verify_identity(build)
        prefix=args.results/tag
        command=[build['binary'],str(file),str(threads),'0',mode,workload,'1',str(prefix),str(replays),str(frames),str(early)]
        env=dict(os.environ)
        for k in ['LD_LIBRARY_PATH','LD_PRELOAD','DYLD_LIBRARY_PATH','EARLY_OUTPUT','FFMPEG_LIVE_IDR_EARLY','FFMPEG_LIVE_POC_EARLY']:
            env.pop(k,None)
        with prefix.with_suffix('.log').open('x') as log:
            ret=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=150).returncode
        verify_identity(build)
        log=prefix.with_suffix('.log').read_text()
        require(ret==0 and re.findall(r'^terminal_return=(-?\d+)$',log,re.M)==['0'],'probe failed: '+tag)
        loaded=re.findall(r'^runtime_avcodec=(.+)$',log,re.M)
        require(len(loaded)==1 and str(Path(loaded[0]).resolve()) in build['libraries'],'wrong loaded libavcodec')
        summary=json.loads(prefix.with_suffix('.json').read_text())
        data=list(csv.DictReader(prefix.with_suffix('.csv').open()))
        require(summary['frames']==len(data)==frames*replays,'wrong fixed declared total')
        require(sorted(int(r['sequence']) for r in data)==list(range(frames*replays)),'missing/duplicate source identity')
        require([int(r['pts']) for r in data]==sorted(int(r['pts']) for r in data),'display order changed')
        if build_name=='candidate':
            observation=json.loads(prefix.with_suffix('.early.json').read_text())
            require(sum(observation['frames'])==frames*replays and observation['pictures']==frames*replays,'wrong complete observations')
            require(sum(observation['decisions'])==observation['pictures'],'wrong AU decisions')
            require(early or sum(observation['frames'][1:])==0,'disabled path selected early frames')
            if early and 'hrd-targets' in str(file):require(observation['frames'][1]>0,'HRD path did not run')
            if early and 'x265' in file.name:require(sum(observation['frames'][3:])>0,'HEVC path did not run')
            if early and file.parent.name=='targets' and 'x264' in file.name:require(sum(observation['frames'][1:])==0,'no-HRD path did not fall back')
            summary['observation']=observation
        digests=list(csv.reader(prefix.with_suffix('.sha256').read_text().splitlines())) if workload=='verify' else None
        if digests is not None:require(len(digests)==frames*replays and all(len(r)==14 for r in digests),'incomplete correctness hashes')
        return dict(case=tag,build=build_name,source=file.name,folder=file.parent.name,mode=mode,threads=threads,early=early,
            workload=workload,replays=replays,input_sha256=sha256(file),command=command,summary=summary,
            ordered_sources=[int(r['sequence']) for r in data] if workload=='verify' else None,
            files={s:sha256(prefix.with_suffix(s)) for s in ['.csv','.json','.log']}),digests
    try:
        for file,frames in inputs:
            folder=file.parent.name
            digest=sha256(file)
            for threads,mode,early in itertools.product([2,12],['legacy','async'],[0,1]):
                label=f'{folder}-{file.stem}-{mode}-t{threads}-e{early}'
                a,oracle=run('baseline',file,frames,mode,threads,0,'verify',3,label+'-oracle')
                b,actual=run('candidate',file,frames,mode,threads,early,'verify',3,label+'-check')
                require(a['ordered_sources']==b['ordered_sources'],'continuous source display order mismatch '+label)
                require(all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(oracle,actual)),'continuous pixel/metadata mismatch '+label)
                rows.extend([a,b])
                # Warmup both builds; timed repetitions alternate execution order.
                for build in builds:run(build,file,frames,mode,threads,early,'perf',3,label+'-warmup-'+build)
                for repeat in range(args.repeats):
                    for build in (['baseline','candidate'] if repeat%2==0 else ['candidate','baseline']):
                        r,_=run(build,file,frames,mode,threads,early,'perf',args.replays,label+f'-r{repeat}-'+build)
                        rows.append(r)
                require(sha256(file)==digest,'source changed')
                (args.results/'runs.json').write_text(json.dumps(rows,indent=2)+'\n')
                print(label,'PASS',flush=True)
        groups={}
        for row in rows:
            if row['workload']!='perf':continue
            key=(row['folder'],row['source'],row['mode'],row['threads'],row['early'],row['build'])
            groups.setdefault(key,[]).append(row['summary'])
        medians=[]
        for key,runs in groups.items():
            medians.append(dict(zip(['folder','source','mode','threads','early','build'],key),
                fps=statistics.median(r['frames']*1e6/r['elapsed_us'] for r in runs),
                cpu_us_per_frame=statistics.median(r['cpu_us']/r['frames'] for r in runs),
                cpu_cores=statistics.median(r['cpu_us']/r['elapsed_us'] for r in runs)))
        (args.results/'summary.json').write_text(json.dumps({'status':'passed','scope':'local codec continuous replay; not service capacity',
            'repeats':args.repeats,'replays':args.replays,'correctness_comparisons':len(inputs)*8,'medians':medians},indent=2)+'\n')
        return 0
    except Exception as error:
        (args.results/'FAILED.json').write_text(json.dumps({'error':str(error),'completed_runs':len(rows)},indent=2)+'\n')
        raise

if __name__=='__main__':sys.exit(main())
