"""Exact crawl parity across membership stores, engines, batching and recovery."""
from pathlib import Path
from contextlib import closing
import signal
import sqlite3
import subprocess
import tempfile
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_fixture import FanoutFixture
from checkpoint_fixture import CheckpointFixture
BINARY=sys.argv[1]
MODES=('memory','sqlite','bloom-sqlite')
def run(*flags,ok=True):
 p=subprocess.run([BINARY,*map(str,flags)],text=True,capture_output=True,timeout=120)
 if ok:assert p.returncode==0,p.stdout+p.stderr
 else:assert p.returncode!=0,p.stdout+p.stderr
 return dict(line.split(': ',1) for line in p.stdout.splitlines() if ': ' in line),p

def flags(site,db,mode,engine='async',batch=100):
 result=['--engine',engine,'--state-db',db,'--dedup',mode,'--db-batch-size',str(batch),'--depth','2','--max-pages','1000','--pending-high-watermark','10','--pending-low-watermark','5','--max-per-host','3',site.url()]
 if engine!='sequential':result+=['--workers' if engine=='threaded' else '--concurrency','8']
 if mode=='bloom-sqlite':result+=['--bloom-expected','16','--bloom-fp-rate','0.5']
 return result

def inspect(db):
 with closing(sqlite3.connect(f'file:{db}?mode=ro',uri=True)) as c:
  c.execute('BEGIN')
  seen={r[0] for r in c.execute('SELECT url FROM seen_urls')}
  pending=c.execute('SELECT COUNT(*) FROM unfinished').fetchone()[0]
  payload=c.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]
  assert not list(c.execute('PRAGMA foreign_key_check'))
 return seen,pending,payload

def verify(site,db,s):
 seen,pending,payload=inspect(db)
 assert len(seen)==site.total and pending==0 and 'completed 1\n' in payload
 assert {site.url(h,p) for h,p in site.snapshot()['counts'] if p!='/robots.txt'}==seen
 assert int(s['URLs discovered'])==site.total and int(s['Peak admitted unfinished'])<=10 and int(s['Deferred discovery URLs'])==0
 assert int(s['Uncommitted seen cache remaining'])==0
 if s['Dedup backend']=='bloom-sqlite':assert int(s['Bloom false positives'])>0 and s['Bloom capacity exceeded']=='yes'

with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp)
 for mode in MODES:
  for engine in ('sequential','threaded','async'):
   with FanoutFixture(4,30,duplicates=True,delay=.001) as site:
    db=root/f'{mode}-{engine}.db'
    s,_=run(*flags(site,db,mode,engine,batch=100000));verify(site,db,s)
    assert all(n==1 for n in site.snapshot()['counts'].values())
    if mode!='memory':assert int(s['Uncommitted seen cache hits'])>0 and int(s['Uncommitted seen cache peak'])<200
    before=site.snapshot()['counts'];_,p=run('resume',db);assert 'already completed' in p.stdout and site.snapshot()['counts']==before
   print(mode,engine,'exact set, concurrent duplicates and precommit cache passed')
  with FanoutFixture(4,30,duplicates=True,delay=.001) as site:
   db=root/f'resume-{mode}.db';s,_=run(*flags(site,db,mode,'threaded',10),'--stop-after-attempts','4')
   assert int(s['Deferred discovery URLs'])>0
   s,_=run('resume',db,'--engine','async','--concurrency','8');verify(site,db,s)
   assert all(n==1 for n in site.snapshot()['counts'].values())
   if mode=='bloom-sqlite':assert float(s['Bloom rebuild time ms'])>0 and float(s['Resume startup time ms'])>0
  with FanoutFixture(4,30,duplicates=True,retry_every=10,delay=.001) as site:
   db=root/f'retry-{mode}.db';s,_=run(*flags(site,db,mode),'--respect-robots','--max-retries','1','--retry-backoff-ms','5','--retry-max-backoff-ms','5','--no-retry-jitter');verify(site,db,s)
   assert int(s['Retries attempted'])==12 and int(s['Recovered after retry'])==12 and int(s['Resources completed'])==site.total
  # Policy-skipped URLs remain exact seen identities after cross-engine resume.
  with CheckpointFixture() as site:
   db=root/f'robots-{mode}.db'
   s,_=run('--engine','sequential','--state-db',db,'--dedup',mode,'--respect-robots','--depth','3','--max-pages','100','--stop-after-attempts','2',site.url())
   _,p=run('resume',db,'--engine','threaded','--workers','8')
   assert site.snapshot()['counts'].get((0,'/private/blocked'),0)==0
   seen,_,payload=inspect(db);assert site.url(0,'/private/blocked') in seen and 'urls_blocked_by_robots 1\n' in payload
 for mode in ('sqlite','bloom-sqlite'):
  with FanoutFixture(500,1) as site:
   db=root/f'unbounded-{mode}.db';command=flags(site,db,mode,'sequential',100)
   start=command.index('--pending-high-watermark');del command[start:start+4]
   s,_=run(*command,'--unbounded-pending','--max-pages','2000','--stop-after-attempts','1')
   assert int(s['Uncommitted seen cache peak'])<=64 and int(s['Deferred discovery URLs'])==436
   s,_=run('resume',db,'--engine','async','--concurrency','8')
   seen,pending,payload=inspect(db);assert len(seen)==site.total and not pending and 'completed 1\n' in payload
   assert all(n==1 for n in site.snapshot()['counts'].values()) and int(s['Uncommitted seen cache remaining'])==0
 # Abrupt loss recovers exact committed membership and rebuilds Bloom.
 with FanoutFixture(10,60,duplicates=True,delay=.02) as site:
  db=root/'crash.db';process=subprocess.Popen([BINARY,*map(str,flags(site,db,'bloom-sqlite','threaded',1))],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   deadline=time.monotonic()+30
   while time.monotonic()<deadline and process.poll() is None:
    try:
     seen,pending,payload=inspect(db)
     if len(seen)>20 and 'extent ' in payload:
      process.send_signal(signal.SIGSTOP);break
    except sqlite3.Error:pass
    time.sleep(.003)
   else:raise AssertionError('No committed Bloom recovery prefix')
   process.kill();_,errors=process.communicate(timeout=10);assert 'WARNING: ThreadSanitizer' not in errors
   s,_=run('resume',db,'--engine','async','--concurrency','8');verify(site,db,s)
   assert site.snapshot()['counts'][(0,'/')]==1
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=10)
 for extra in (('--dedup','invalid'),('--dedup','sqlite'),('--dedup','bloom-sqlite'),('--bloom-expected','0'),('--bloom-expected','-1'),('--bloom-fp-rate','0'),('--bloom-fp-rate','1'),('--bloom-fp-rate','nan'),('--bloom-fp-rate','inf'),('--bloom-fp-rate','-0.1'),('--seen-reserve','0')):
  run(*extra,'--max-pages','0','http://127.0.0.1:1/',ok=False)
 run('--dedup','sqlite','--checkpoint',root/'bad.chk','http://127.0.0.1:1/',ok=False)
print('Exact cross-backend crawl, retries, policy, backpressure, clean/crash resume and CLI checks passed')
