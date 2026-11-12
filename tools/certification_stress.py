"""Manual wide-concurrency certification; exact work checks, no timing target."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from backpressure_fixture import FanoutFixture
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path('build-release/fluxcrawl'));p.add_argument('--repeats',type=int,default=2);a=p.parse_args()
    if a.repeats<1:p.error('positive repeat count required')
    for repeat in range(a.repeats):
      for engine in ('threaded','async'):
        with FanoutFixture(20,100,delay=.20,duplicates=True,retry_every=17) as site:
            command=[str(a.binary.resolve()),'--engine',engine,'--workers' if engine=='threaded' else '--concurrency','32',
                '--max-per-host','16','--rate-per-host','100','--burst-per-host','16','--pending-high-watermark','128','--pending-low-watermark','32','--respect-robots',
                '--max-retries','1','--retry-backoff-ms','10','--no-retry-jitter','--depth','2','--max-pages','2300',site.url()]
            result=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,NO_PROXY='*',no_proxy='*'),timeout=60)
            assert result.returncode==0,result.stdout+result.stderr
            summary=dict(line.split(': ',1) for line in result.stdout.splitlines() if ': ' in line)
            state=site.snapshot();counts=state['counts'];normal={key:n for key,n in counts.items() if key[1]!='/robots.txt'}
            expected={(0,'/')}|{(b%3,f'/branch/{b}') for b in range(20)}|{((b+n)%3,f'/leaf/{b}/{n}') for b in range(20) for n in range(100)}
            assert set(normal)==expected and len(normal)==2021
            assert sum(normal.values())==2139 and int(summary['Pages attempted'])==2139
            for field in ['Retries scheduled','Retries attempted','Recovered after retry']:assert int(summary[field])==118
            assert int(summary['URLs discovered'])==2021 and int(summary['URLs pending'])==0 and int(summary['Deferred discovery URLs'])==0
            assert int(summary['Robots fetches'])==3 and int(summary['Robots fetch failures'])==0
            assert int(summary['Peak admitted unfinished'])<=128 and int(summary['Backpressure activations'])>0
            assert max(state['peak_per_origin'])<=16 and 16<=state['global_peak']<=32,state['global_peak']
            print(json.dumps(dict(engine=engine,round=repeat+1,concurrency=32,delay_ms=200,rate_per_host=100,burst=16,logical_urls=2021,attempts=2139,recovered=118,
                global_peak=state['global_peak'],per_origin_peak=state['peak_per_origin'],peak_unfinished=int(summary['Peak admitted unfinished']))),flush=True)
