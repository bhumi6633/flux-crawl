"""Dedicated high-fanout-backpressure experiment; excludes headline network benchmarks."""
import argparse
from contextlib import closing
import csv
from pathlib import Path
import platform
import re
import sqlite3
import statistics
import subprocess
import tempfile
import time
from backpressure_fixture import FanoutFixture

def trial(binary, engine, high, low, persistent, branches, leaves, retries, robots, budget=None):
    with tempfile.TemporaryDirectory() as tmp, FanoutFixture(branches,leaves,retry_every=retries) as site:
        db=Path(tmp)/'crawl.db'
        command=[str(binary),'--engine',engine,'--depth','2','--max-pages',str(budget if budget is not None else site.total+((branches*leaves+retries-1)//retries if retries else 0)),'--max-per-host','4']
        if engine!='sequential':command+=['--workers' if engine=='threaded' else '--concurrency','8']
        command+=['--unbounded-pending'] if high is None else ['--pending-high-watermark',str(high),'--pending-low-watermark',str(low)]
        if persistent:command+=['--state-db',str(db),'--db-batch-size','100']
        if robots:command+=['--respect-robots']
        if retries:command+=['--max-retries','1','--retry-backoff-ms','5','--retry-max-backoff-ms','5','--no-retry-jitter']
        command+=[site.url()]
        # External process RSS, no platform-specific production instrumentation.
        measured=platform.system()=='Darwin' and Path('/usr/bin/time').exists()
        if measured:command=['/usr/bin/time','-l',*command]
        began=time.monotonic();p=subprocess.run(command,text=True,capture_output=True,timeout=240);elapsed=time.monotonic()-began
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        summary=dict(line.split(': ',1) for line in p.stdout.splitlines() if ': ' in line)
        if measured:
            match=re.search(r'(\d+)\s+maximum resident set size',p.stderr)
            assert match,p.stderr
            rss=int(match[1])
        else:rss=''
        complete=budget is None
        counts=site.snapshot()['counts'];normal={key:n for key,n in counts.items() if key[1]!='/robots.txt'}
        assert int(summary['Fetch failures'])==0
        if complete:
            expected={(0,'/')}|{(b%3,f'/branch/{b}') for b in range(branches)}|{((b+n)%3,f'/leaf/{b}/{n}') for b in range(branches) for n in range(leaves)}
            assert set(normal)==expected and int(summary['URLs discovered'])==site.total
            assert int(summary['URLs pending'])==0 and int(summary['Deferred discovery URLs'])==0
            retry_count=sum(n-1 for n in normal.values())
            assert retry_count==(sum((b*leaves+n)%retries==0 for b in range(branches) for n in range(leaves)) if retries else 0)
            assert int(summary['Pages attempted'])==site.total+retry_count
            if retries:assert int(summary['Retries attempted'])==retry_count and int(summary['Recovered after retry'])==retry_count and int(summary['Retries exhausted'])==0
            if robots:assert int(summary['Robots fetches'])==3 and int(summary['Robots fetch failures'])==0
        else:assert int(summary['Pages attempted'])==budget
        if high is not None:
            assert int(summary['Peak pending URLs'])<=high and int(summary['Peak admitted unfinished'])<=high
        assert max(site.snapshot()['peak_per_origin'])<=4
        if persistent:
            with closing(sqlite3.connect(f'file:{db}?mode=ro',uri=True)) as c:
                assert not list(c.execute('PRAGMA foreign_key_check'))
                if complete:
                    assert c.execute('SELECT COUNT(*) FROM seen_urls').fetchone()[0]==site.total
                    assert c.execute('SELECT COUNT(*) FROM unfinished').fetchone()[0]==0
                    assert 'completed 1\n' in c.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]
            assert int(summary['SQLite peak queued batches'])<=(8 if engine=='threaded' else 1)
            assert int(summary['SQLite peak buffered mutations'])<4096 if high is not None else True
        row=dict(scenario='high-fanout-backpressure',engine=engine,branches=branches,leaves_per_branch=leaves,logical_urls=site.total,
                 pending_high=high or 0,pending_low=low if high else 0,persistence_mode='sqlite' if persistent else 'none',batch_size=100 if persistent else 0,
                 http_protocol='HTTP/1.1',concurrency=1 if engine=='sequential' else 8,max_per_host=4,retry_every=retries,respect_robots=robots,
                 attempt_budget=budget or 'full',elapsed_ms=elapsed*1000,requests_per_second=int(summary['Pages attempted'])/elapsed,peak_rss_bytes=rss)
        for label,key in [('Peak pending URLs','peak_pending'),('Peak admitted unfinished','peak_unfinished'),('Peak seen URLs','peak_seen'),('Peak deferred discoveries','peak_deferred'),('Backpressure activations','activations'),('Time under pressure ms','pressure_ms'),('Deferred spool I/O time ms','deferred_io_ms'),('SQLite commit time ms','commit_ms'),('SQLite peak buffered mutations','buffered_mutations_peak'),('SQLite peak queued batches','queued_batches_peak')]:
            row[key]=float(summary.get(label,0))
        row['database_bytes']=db.stat().st_size if persistent else 0
        return row,p.stdout

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,default=Path('build-release/fluxcrawl'))
    p.add_argument('--engine',choices=['sequential','threaded','async','all'],default='async')
    p.add_argument('--branches',type=int,default=20);p.add_argument('--leaves',type=int,default=500)
    p.add_argument('--high',type=int,default=1000);p.add_argument('--low',type=int,default=750)
    p.add_argument('--runs',type=int,default=3);p.add_argument('--warmups',type=int,default=1)
    p.add_argument('--retry-every',type=int,default=0);p.add_argument('--robots',action='store_true');p.add_argument('--sqlite',action='store_true')
    p.add_argument('--budget',type=int);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if min(a.branches,a.leaves,a.high,a.runs)<=0 or not 0<=a.low<a.high or a.warmups<0 or a.retry_every<0 or a.output.exists():p.error('positive workload/runs/high, 0<=low<high, fresh output path required')
    rows=[];engines=('sequential','threaded','async') if a.engine=='all' else (a.engine,)
    for run in range(-a.warmups,a.runs):
        for engine in engines:
            for high,low in ((None,0),(a.high,a.low),(100,50)):
                row,summary=trial(a.binary.resolve(),engine,high,low,a.sqlite,a.branches,a.leaves,a.retry_every,a.robots,a.budget)
                if run>=0:
                    row['run']=run+1;rows.append(row)
                    print(engine,'high',high,'run',run+1,'elapsed',round(row['elapsed_ms'],3),'peak_pending',row['peak_pending'],'RSS',row['peak_rss_bytes'],flush=True)
    with a.output.open('x',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    for engine in engines:
        for high in (0,a.high,100):
            selected=[r for r in rows if r['engine']==engine and r['pending_high']==high]
            print(engine,'high',high,{k:statistics.median(r[k] for r in selected) for k in ('elapsed_ms','requests_per_second','peak_pending','peak_unfinished','peak_rss_bytes','activations','pressure_ms','deferred_io_ms','commit_ms','buffered_mutations_peak','queued_batches_peak') if selected[0][k]!=''})

if __name__=='__main__':main()
