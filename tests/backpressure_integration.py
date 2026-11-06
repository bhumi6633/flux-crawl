"""Tiny-cap losslessness, concurrency, policy/retry, and deferred recovery checks."""
from pathlib import Path
from contextlib import closing
import sqlite3
import subprocess
import tempfile
import sys
import time
import signal
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_fixture import FanoutFixture
BINARY=sys.argv[1]

def run(*args):
    p=subprocess.run([BINARY,*map(str,args)],text=True,capture_output=True,timeout=100)
    assert p.returncode==0,p.stdout+p.stderr
    return dict(line.split(': ',1) for line in p.stdout.splitlines() if ': ' in line),p.stdout

def args(site,engine,high=20,low=10):
    flags=['--engine',engine,'--depth','2','--max-pages',str(site.total+100),'--max-per-host','3']
    if engine!='sequential':flags+=['--workers' if engine=='threaded' else '--concurrency','8']
    flags+=['--pending-high-watermark',str(high),'--pending-low-watermark',str(low),site.url()]
    return flags

def expected(site):
    return {(0,'/')}|{(b%3,f'/branch/{b}') for b in range(site.branches)}|{((b+n)%3,f'/leaf/{b}/{n}') for b in range(site.branches) for n in range(site.leaves)}

def verify(summary,site,high):
    assert int(summary['URLs discovered'])==site.total and int(summary['URLs pending'])==0 and int(summary['Deferred discovery URLs'])==0,summary
    assert int(summary['Peak pending URLs'])<=high and int(summary['Peak admitted unfinished'])<=high,summary
    assert int(summary['Backpressure activations'])>0 and int(summary['Admission resumptions'])>0,summary
    assert int(summary['Peak deferred discoveries'])>high,summary
    assert max(site.snapshot()['peak_per_origin'])<=3

def metadata(db):
    with closing(sqlite3.connect(f'file:{db}?mode=ro',uri=True)) as c:
        return c.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]

with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    for engine in ('sequential','threaded','async'):
        with FanoutFixture(6,40,duplicates=True,delay=.001) as site:
            summary,_=run(*args(site,engine));verify(summary,site,20)
            counts=site.snapshot()['counts'];assert set(counts)==expected(site) and all(n==1 for n in counts.values())
        # Unrestricted reference uses the identical logical graph.
        with FanoutFixture(6,40,duplicates=True) as site:
            flags=args(site,engine);del flags[flags.index('--pending-high-watermark'):flags.index('--pending-low-watermark')+2]
            summary,_=run(*flags,'--unbounded-pending')
            assert set(site.snapshot()['counts'])==expected(site) and int(summary['URLs discovered'])==site.total
        print(engine,'bounded/unbounded exact URL-set equivalence, duplicate and progress passed')
    # One completed document can exceed capacity by orders of magnitude.
    for engine in ('sequential','threaded','async'):
        with FanoutFixture(2000,1) as site:
            summary,_=run(*args(site,engine), '--max-pages','1')
            assert int(summary['Pages attempted'])==1 and int(summary['URLs discovered'])==21
            assert int(summary['URLs pending'])==20 and int(summary['Deferred discovery URLs'])==1980
            assert int(summary['Peak admitted unfinished'])==20
    # Stronger-than-queue reservation means retries always retain a lifecycle slot.
    for engine in ('sequential','threaded','async'):
        with FanoutFixture(4,30,retry_every=10,delay=.001) as site:
            summary,_=run(*args(site,engine,10,5),'--respect-robots','--max-retries','1','--retry-backoff-ms','5','--retry-max-backoff-ms','5','--no-retry-jitter')
            verify(summary,site,10);counts=site.snapshot()['counts']
            assert sum(n-1 for (h,p),n in counts.items() if p.startswith('/leaf/'))==12
            assert all(n==1 for (h,p),n in counts.items() if p=='/robots.txt') and sum(p=='/robots.txt' for h,p in counts)==3
    for backend,initial,resumed in (('sqlite','threaded','async'),('snapshot','async','threaded')):
        with FanoutFixture(6,40,duplicates=True,delay=.002) as site:
            path=root/('crawl.db' if backend=='sqlite' else 'crawl.chk')
            flag='--state-db' if backend=='sqlite' else '--checkpoint'
            summary,_=run(*args(site,initial),flag,path,'--stop-after-attempts','4')
            assert int(summary['Deferred discovery URLs'])>0 and int(summary['Peak admitted unfinished'])<=20
            if backend=='sqlite':
                payload=metadata(path);assert 'extent ' in payload and 'completed 0\n' in payload
            else:assert 'extent ' in path.read_text()
            summary,_=run('resume',path,'--engine',resumed,'--concurrency' if resumed=='async' else '--workers','8')
            verify(summary,site,20);counts=site.snapshot()['counts']
            assert set(counts)==expected(site) and all(n==1 for n in counts.values()),counts
            print(backend,'cross-engine deferred resume completed losslessly')
    with FanoutFixture(10,60,delay=.015) as site:
        path=root/'crash.db'
        process=subprocess.Popen([BINARY,*map(str,args(site,'threaded',10,5)),'--state-db',str(path),'--db-batch-size','1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and process.poll() is None:
                try:
                    payload=metadata(path)
                    if 'extent ' in payload and 'completed 0\n' in payload:
                        process.send_signal(signal.SIGSTOP);break
                except sqlite3.Error:pass
                time.sleep(.003)
            else:raise AssertionError('No durable deferred crash boundary')
            process.kill();_,errors=process.communicate(timeout=10)
            assert 'WARNING: ThreadSanitizer' not in errors,errors
            summary,_=run('resume',path,'--engine','async','--concurrency','8')
            assert int(summary['URLs discovered'])==site.total and int(summary['Deferred discovery URLs'])==0
            assert int(summary['Peak admitted unfinished'])<=10 and 'completed 1\n' in metadata(path)
            assert set(site.snapshot()['counts'])==expected(site) and site.snapshot()['counts'][(0,'/')]==1
        finally:
            if process.poll() is None:process.kill();process.communicate(timeout=10)
    # Depth barriers remain intact even when much of the shallow frontier is on disk.
    with FanoutFixture(12,20,delay=.001) as site:
        summary,_=run(*args(site,'async',5,2))
        starts=site.snapshot()['starts'];first_leaf=next(i for i,(_,p,_) in enumerate(starts) if p.startswith('/leaf/'))
        assert first_leaf>=13 and int(summary['Peak admitted unfinished'])<=5
    class Skewed(FanoutFixture):
        @property
        def total(self): return 210
        def response(self,h,p):
            if p in ('/branch/1','/branch/2'):
                b=int(p[-1]);return 200,{'Content-Type':'text/html'},(''.join(f'<a href="{self.url(b,f"/leaf/{b}/{n}")}">link</a>' for n in range(3))).encode()
            return super().response(h,p)
    with Skewed(3,200,delay=.001,skewed=True) as site:
        summary,_=run(*args(site,'async',10,5));verify(summary,site,10)
        starts=site.snapshot()['starts']
        for small in (1,2):
            first=next(i for i,(h,p,_) in enumerate(starts) if h==small and p.startswith('/leaf/'))
            assert sum(h==0 and p.startswith('/leaf/') for h,p,_ in starts[:first])<50
    # A missing companion fails before requests; it cannot silently erase deferred work.
    with FanoutFixture(4,30) as site:
        path=root/'corrupt.chk';run(*args(site,'sequential',10,5),'--checkpoint',path,'--stop-after-attempts','2')
        import shlex
        spool=next(line for line in path.read_text().splitlines() if line.startswith('spool '));directory=Path(shlex.split(spool)[1])
        extent=shlex.split(next(line for line in path.read_text().splitlines() if line.startswith('extent ')))
        (directory/(extent[5]+'.links')).write_bytes(b'broken')
        before=site.snapshot()['counts'];p=subprocess.run([BINARY,'resume',str(path)],capture_output=True,text=True,timeout=15)
        assert p.returncode!=0 and site.snapshot()['counts']==before
    for flags in (('--pending-high-watermark','0'),('--pending-high-watermark','-1'),('--pending-high-watermark','184467440737095516160'),('--pending-low-watermark','10000'),('--pending-high-watermark','10','--pending-low-watermark','10'),('--unbounded-pending','--pending-low-watermark','1')):
        p=subprocess.run([BINARY,*flags,'--max-pages','0','http://127.0.0.1:1/'],capture_output=True,timeout=10)
        assert p.returncode!=0
print('Backpressure tiny-bound, retry, robots, dedup, persistence and CLI integration passed')
