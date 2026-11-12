"""Bounded parser fuzz certification; logs/artifacts live in an ignored build tree."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,default=Path('build-fuzz'));p.add_argument('--seconds',type=int,default=15)
    a=p.parse_args(); assert a.seconds>0
    root=Path(__file__).resolve().parents[1]
    for target in ('url','resolve','robots','checkpoint'):
        corpus=a.build/'fuzz'/'smoke-corpus'/target;corpus.mkdir(parents=True,exist_ok=True)
        shutil.copytree(root/'fuzz'/'corpus'/target,corpus,dirs_exist_ok=True)
        artifacts=a.build/'fuzz'/'artifacts'/target;artifacts.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ);env['ASAN_OPTIONS']=env.get('ASAN_OPTIONS','quarantine_size_mb=32')
        result=subprocess.run([str(a.build/'fuzz'/('fluxcrawl_fuzz_'+target)),str(corpus),'-max_total_time='+str(a.seconds),
            '-max_len=16384','-timeout=5','-rss_limit_mb=512','-print_final_stats=1','-artifact_prefix='+str(artifacts)+'/'],capture_output=True,text=True,env=env,timeout=a.seconds+15)
        (artifacts/'smoke.log').write_text(result.stdout+result.stderr)
        count=re.search(r'stat::number_of_executed_units:\s*(\d+)',result.stderr)
        print(json.dumps({'target':target,'seconds':a.seconds,'executions':int(count[1]) if count else None,'returncode':result.returncode,'artifacts':len([x for x in artifacts.iterdir() if x.name!='smoke.log'])}),flush=True)
        if result.returncode: raise SystemExit(result.returncode)
