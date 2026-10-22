from pathlib import Path
import subprocess,json,time
p=Path(__file__).resolve().parents[2]
for script in ['run_hrd_targets_proof_final.py','run_targets_proof_final.py','run_performance.py']:
 (p/'out/phase2/current-stage.json').write_text(json.dumps(dict(stage=script,started=time.time()))+'\n')
 with (p/'out/phase2'/('final-'+script+'.log')).open('w') as f:subprocess.run(['python3',str(p/'research/phase2'/script)],stdout=f,stderr=subprocess.STDOUT,check=True)
(p/'out/phase2/current-stage.json').write_text('{"stage":"complete"}\n')
