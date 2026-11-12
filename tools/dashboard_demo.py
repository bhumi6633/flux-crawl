"""Manual, deterministic ~40s dashboard demo. Never run by CTest."""
import argparse
import subprocess
import tempfile
from backpressure_fixture import FanoutFixture

class Demo(FanoutFixture):
    def response(self,origin,path):
        if path=='/robots.txt':
            return 200,{'Content-Type':'text/plain'},b'User-agent: *\nDisallow: /leaf/0/\n'
        return super().response(origin,path)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='./build-release/fluxcrawl')
    parser.add_argument('--port',type=int,default=8080)
    parser.add_argument('--linger-ms',type=int,default=15000)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fluxcrawl-demo-') as tmp, Demo(12,60,delay=.18,retry_every=17,duplicates=True) as site:
        site.delays=(.12,.20,.28)
        subprocess.run([args.binary,'--engine','async','--concurrency','8','--max-per-host','2',
            '--dashboard','--dashboard-port',str(args.port),'--dashboard-linger-ms',str(args.linger_ms),'--dashboard-topology',
            '--max-pages','2000','--depth','2','--respect-robots','--max-retries','2','--no-retry-jitter',
            '--pending-high-watermark','60','--pending-low-watermark','15','--dedup','bloom-sqlite',
            '--bloom-expected','300','--bloom-fp-rate','0.05','--state-db',tmp+'/demo.db','--db-batch-size','24',site.url()],check=True)
