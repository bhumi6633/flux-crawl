"""Official Release-only architecture profiles; advanced feature runs stay separate."""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import subprocess
import threading
from benchmark_server import BenchmarkServer,scenario_workload
PROFILES={'latency-bound':'latency-bound','processing-heavy':'link-heavy','high-fanout':'high-fanout','mixed-latency':'mixed-latency'}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path('build-release/fluxcrawl'))
    p.add_argument('--output',type=Path,default=Path('certification-results/benchmarks'));p.add_argument('--runs',type=int,default=3);p.add_argument('--warmups',type=int,default=1)
    a=p.parse_args()
    if a.runs<1 or a.warmups<0 or a.output.exists():p.error('positive runs, nonnegative warmups, and new output directory required')
    a.output.mkdir(parents=True);binary=a.binary.resolve()
    env_output=subprocess.check_output([str(binary.parent/'fluxcrawl_certification_environment')],text=True)
    metadata=dict(line.split('\t',1) for line in env_output.splitlines())
    metadata.update(os=platform.system(),os_release=platform.release(),architecture=platform.machine(),logical_cores=os.cpu_count(),fixture_python=platform.python_version(),build='Release',runs=a.runs,warmups=a.warmups,parallelism=[1,4,16],pages=100)
    try:
        revision=subprocess.run(['git','rev-parse','HEAD'],capture_output=True,text=True)
        metadata['commit']=revision.stdout.strip() if revision.returncode==0 else 'source archive (Git unavailable)'
    except FileNotFoundError:
        metadata['commit']='source archive (Git unavailable)'
    cpu=subprocess.run(['sysctl','-n','machdep.cpu.brand_string'],capture_output=True,text=True) if platform.system()=='Darwin' else None
    metadata['cpu_model']=cpu.stdout.strip() if cpu and cpu.returncode==0 else platform.processor() or 'unavailable'
    (a.output/'environment.json').write_text(json.dumps(metadata,indent=2)+'\n')
    all_rows=[]
    for name,scenario in PROFILES.items():
        workload=scenario_workload(scenario)
        with BenchmarkServer(0,workload) as server:
            thread=threading.Thread(target=server.serve_forever);thread.start()
            try:
                dest=a.output/(name+'.csv')
                result=subprocess.run([str(binary),'benchmark','--seed',f'http://127.0.0.1:{server.server_port}/page/0',
                    '--scenario',scenario,'--engine','all','--concurrency','1,4,16','--workers','1,4,16',
                    '--runs',str(a.runs),'--warmups',str(a.warmups),'--output',str(dest)],text=True,capture_output=True,timeout=300)
                if result.returncode:raise RuntimeError(result.stdout+result.stderr)
                (a.output/(name+'.txt')).write_text(result.stdout)
                with dest.open() as f:rows=list(csv.DictReader(f))
                assert all(r['build_type']=='Release' for r in rows),'benchmark requires Release'
                assert all(int(r['pages_attempted'])==100 and int(r['fetch_failures'])==0 for r in rows)
                for row in rows:row['certification_profile']=name
                all_rows+=rows
                print(name,'validated samples',len(rows),flush=True)
            finally:server.shutdown();thread.join()
    with (a.output/'samples.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(all_rows[0]));writer.writeheader();writer.writerows(all_rows)
