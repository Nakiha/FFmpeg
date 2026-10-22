from pathlib import Path
import json,subprocess,hashlib
p=Path(__file__).resolve().parents[1]
def read(s):return json.loads((p/s).read_text())
a=read('out/acceptance.json');assert a['status']=='rejected' and not a['accepted_H264_optimization']
m=read('out/matrix/summary-with-returns.json');assert len(m)==120 and all(x['baseline_vs_off'] for x in m)
for x in m:
 assert x['baseline']['exact_terminal_return']==x['candidate_off']['exact_terminal_return']
 if x['input']=='pcm-poc2-invalid-leading.h264':assert x['counts']==[5,5,4] and x['candidate_on']['exact_terminal_return']==[0]
b=read('out/boundaries/summary-with-returns.json')
for x in b:assert x['off']['exact_terminal_return']==x['on']['exact_terminal_return']
last=read('out/last-regressions/summary.json');assert all(x['exact'] for x in last)
assert all(x['ordered_pixels_equal'] for x in read('out/final-checks/corrected-oracles.json'))
r=read('out/perf-v2/results.json');assert len(r)==50 and all(x['returncode']==0 for x in r)
for repo in ['FFmpeg','NakiLiveTranscode']:assert not subprocess.check_output(['git','-C',str(p/repo),'status','--porcelain'],text=True)
identity=read('out/evidence/build-identity.json');assert hashlib.sha256((p/'research/h264-poc2-experimental.patch').read_bytes()).hexdigest()==identity['patch_sha256']
result=dict(investigation_verification='passed',candidate_acceptance='rejected',baseline_off_groups=120,unsafe_counterexample_paths=5,performance_runs=50,source_trees_clean=True)
(p/'out/verification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
