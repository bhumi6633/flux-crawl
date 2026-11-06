"""Opt-in 100k-resource Bloom/SQLite bounded crawl with clean restart."""
from pathlib import Path
from contextlib import closing
import sqlite3,subprocess,tempfile,sys,time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_fixture import FanoutFixture
binary=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as tmp,FanoutFixture(100,1000) as site:
 db=Path(tmp)/'crawl.db'
 command=[binary,'--engine','async','--concurrency','8','--max-per-host','4','--depth','2','--max-pages',str(site.total),'--state-db',str(db),'--dedup','bloom-sqlite','--bloom-expected',str(site.total),'--bloom-fp-rate','.01','--pending-high-watermark','1000','--pending-low-watermark','750','--stop-after-attempts','32',site.url()]
 p=subprocess.run(command,capture_output=True,text=True,timeout=120);assert p.returncode==0,p.stdout+p.stderr
 began=time.monotonic();p=subprocess.run([binary,'resume',str(db),'--engine','async','--concurrency','8'],capture_output=True,text=True,timeout=600);elapsed=time.monotonic()-began
 assert p.returncode==0,p.stdout+p.stderr
 s=dict(line.split(': ',1) for line in p.stdout.splitlines() if ': ' in line)
 assert int(s['URLs discovered'])==site.total and int(s['Pages attempted'])==site.total and int(s['URLs pending'])==0
 assert int(s['Peak admitted unfinished'])<=1000 and int(s['Uncommitted seen cache peak'])<4096 and int(s['Uncommitted seen cache remaining'])==0
 counts=site.snapshot()['counts'];assert len(counts)==site.total and all(n==1 for n in counts.values())
 assert max(site.snapshot()['peak_per_origin'])<=4
 with closing(sqlite3.connect(f'file:{db}?mode=ro',uri=True)) as c:
  assert c.execute('SELECT COUNT(*) FROM seen_urls').fetchone()[0]==site.total and c.execute('SELECT COUNT(*) FROM unfinished').fetchone()[0]==0
  assert not list(c.execute('PRAGMA foreign_key_check')) and 'completed 1\n' in c.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]
 print('100,101 logical URLs, exact fetch counts, bounded queue/cache, Bloom rebuild and restart: PASSED')
 print('Resume process elapsed ms:',elapsed*1000)
 print(p.stdout)
