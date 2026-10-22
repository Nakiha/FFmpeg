#!/usr/bin/env python3
"""Fail-closed H264 boundary/error/preparation-failure checks. New outputs only."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from evidence import Expectation, compare, require, sha256, validate
from run_representatives import existing_input, verify_identity


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workspace', required=True, type=Path)
    ap.add_argument('--identity', required=True, type=Path)
    ap.add_argument('--results', required=True, type=Path)
    ap.add_argument('--interposer', type=Path)
    ap.add_argument('--unstripped-library', type=Path)
    args = ap.parse_args()
    builds = json.loads(args.identity.read_text())['builds']
    args.results.mkdir(parents=True, exist_ok=False)
    outcomes = []
    def run(build, file, label, early, mode, threads, extra=None, cycles=1, expected=192, errors=False):
        verify_identity(build)
        prefix=args.results / label
        env=dict(os.environ)
        for key in ['EARLY_OUTPUT','H264_CERTIFICATE','LD_PRELOAD','LD_LIBRARY_PATH','AU_MUTATION','MUTATION_AU',
                    'SYNTHETIC_PTS','STRICT_ERRORS','NO_HASH','DUMP_MUTATED_AU']:
            env.pop(key,None)
        env.update(EARLY_OUTPUT=str(early), **(extra or {}))
        command=[build['binary'],str(file),str(threads),'0',mode,str(prefix),str(cycles),'0','1']
        with prefix.with_suffix('.log').open('x') as log:
            ret=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=50).returncode
        verify_identity(build)
        log=prefix.with_suffix('.log').read_text()
        loaded=re.findall(r'^runtime_avcodec=(.+)$',log,re.M)
        require(len(loaded)==1 and str(Path(loaded[0]).resolve()) in build['libraries'],'wrong loaded SDK')
        require(not re.search(r'ERROR: AddressSanitizer|runtime error:|LeakSanitizer',log),'sanitizer report')
        if errors:
            require(ret==1 and len(re.findall(r'^terminal_return=-[0-9]+$',log,re.M))==1,'expected a decoder error')
            rows=list(csv.reader(prefix.with_suffix('.sha256').read_text().splitlines()))
            require(all(len(row)==19 for row in rows),'malformed failed-prefix evidence')
        else:
            rows=validate(prefix,ret,Expectation(cycles,expected))
        return rows,log,dict(command=command,env={k:env[k] for k in (extra or {})},returncode=ret,
            files={s:sha256(prefix.with_suffix(s)) for s in ['.log','.csv','.sha256']})
    def check(label, action):
        row={'case':label,'passed':False}
        try:
            row.update(action());row['passed']=True
        except Exception as error:
            row['error']=str(error)
        outcomes.append(row)
        (args.results/'summary.json').write_text(json.dumps(outcomes,indent=2)+'\n')
        print(label,'PASS' if row['passed'] else 'FAIL '+row['error'],flush=True)
    manifest=json.loads((args.workspace/'out/phase3/fixtures/boundary-manifest.json').read_text())
    for fixture in manifest:
        file=args.workspace/'out/phase3/fixtures'/Path(fixture['file']).name
        require(sha256(file)==fixture['sha256'],'boundary fixture changed')
        expected=len(fixture['frames']) if 'frames' in fixture else int(fixture['command'][fixture['command'].index('-frames:v')+1])
        synthetic={'SYNTHETIC_PTS':'1'} if 'frames' in fixture else {} # Declared generator counts, never observed output.
        for mode,threads in [('legacy',1),('legacy',12),('async',2),('async',12)]:
            label=f'{file.stem}-{mode}-t{threads}'
            def action(file=file,expected=expected,mode=mode,threads=threads,label=label,synthetic=synthetic):
                a,_,ra=run(builds['baseline'],file,label+'-off',0,mode,threads,synthetic,3,expected)
                b,_,rb=run(builds['candidate'],file,label+'-on',1,mode,threads,synthetic,3,expected)
                return dict(reference=ra,result=rb,comparison=compare(a,b,Expectation(3,expected),Expectation(3,expected),True))
            check(label,action)
    source,expected=existing_input(args.workspace,'hrd-targets','x264-fixed3-pyr1-open0.mp4')
    oracle,_,_=run(builds['baseline'],source,'full-oracle',0,'legacy',1,expected=expected)
    for mutation in ['late','duplicate','contradictory','short','truncated','parser-error']:
        for strict in [False,True]:
            for mode,threads in [('legacy',1),('async',2),('async',12)]:
                label=f'{mutation}-strict{int(strict)}-{mode}-t{threads}'
                error=strict and mutation in ['truncated','parser-error']
                def action(label=label,mutation=mutation,strict=strict,mode=mode,threads=threads,error=error):
                    extra={'AU_MUTATION':mutation,'MUTATION_AU':'12'}
                    if strict:extra['STRICT_ERRORS']='1'
                    a,_,ra=run(builds['baseline'],source,label+'-off',0,mode,threads,extra,expected=expected,errors=error)
                    b,_,rb=run(builds['candidate'],source,label+'-on',1,mode,threads,extra,expected=expected,errors=error)
                    if error:
                        for rows in [a,b]:
                            require(0<len(rows)<=12,'failed decode did not deliver a bounded valid prefix')
                            require(all(int(r[18])<12 for r in rows),'failed or subsequent AU escaped')
                            require(all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(rows,oracle)),'error output is not the independent full oracle prefix')
                    else:compare(a,b,Expectation(1,expected),Expectation(1,expected),True)
                    return dict(reference=ra,result=rb,expected_error=error,counts=[len(a),len(b)])
                check(label,action)
    if args.interposer:
        require(args.unstripped_library is not None,'matching unstripped library required')
        nm=subprocess.check_output(['nm','-an',str(args.unstripped_library)],text=True)
        symbols=[line.split() for line in nm.splitlines() if re.match(r'^[0-9a-f]+ [tT] ',line)]
        i=next(i for i,s in enumerate(symbols) if s[2]=='ff_h264_live_retire')
        lo,hi=symbols[i][0],symbols[i+1][0]
        for mode,threads in [('legacy',1),('async',2),('async',12)]:
            for nth in [1,2,10,100]:
                label=f'presentation-oom-{mode}-t{threads}-n{nth}'
                def action(label=label,mode=mode,threads=threads,nth=nth):
                    extra={'LD_PRELOAD':str(args.interposer.resolve()),'FAIL_PRESENTATION_LO':lo,'FAIL_PRESENTATION_HI':hi,'FAIL_PRESENTATION_NTH':str(nth)}
                    b,log,rb=run(builds['candidate'],source,label,1,mode,threads,extra,expected=expected)
                    require(log.count('presentation_allocation_failure_injected=1')==1,'allocation failure was not injected exactly once')
                    require(re.search(r'early_stats cycle=0 .*allocation_failures=1$',log,re.M),'decoder did not observe preparation failure')
                    compare(oracle,b,Expectation(1,expected),Expectation(1,expected),True)
                    return dict(result=rb,interposer_sha256=sha256(args.interposer),caller_range=[lo,hi])
                check(label,action)
    return 0 if outcomes and all(r['passed'] for r in outcomes) else 1

if __name__=='__main__':
    sys.exit(main())
