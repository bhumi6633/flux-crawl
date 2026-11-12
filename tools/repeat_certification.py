"""Stop on the first normal-suite failure; preserve each run's complete output."""
import argparse
from pathlib import Path
import subprocess
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',default='build');p.add_argument('--rounds',type=int,default=3);p.add_argument('--output',type=Path,default=Path('certification-results/repeat'));a=p.parse_args()
    if a.rounds<1:p.error('positive round count required')
    a.output.mkdir(parents=True,exist_ok=True)
    for n in range(a.rounds):
        with (a.output/f'normal-{n+1}.log').open('w') as log:
            result=subprocess.run(['ctest','--test-dir',a.build,'--output-on-failure','-j','2'],stdout=log,stderr=subprocess.STDOUT)
        print(n+1,result.returncode,flush=True)
        if result.returncode:raise SystemExit(result.returncode)
