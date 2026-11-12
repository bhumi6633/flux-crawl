"""Bounded graph, repeated engine/server lifecycles in ONE child process. Manual soak."""
import argparse
import json
import subprocess
from backpressure_fixture import FanoutFixture
class Site(FanoutFixture):
    def response(self,origin,path):
        with self.lock:
            if len(self.starts)>1024: self.starts.clear(); self.user_agents.clear()
        if path=='/robots.txt': return 200,{'Content-Type':'text/plain'},b'User-agent: *\nDisallow: /leaf/0/\n'
        return super().response(origin,path)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--driver',default='./build-release/tests/fluxcrawl_certification_driver')
    p.add_argument('--seconds',type=int,default=600)
    p.add_argument('--iterations',type=int,default=0)
    p.add_argument('--workers',type=int,default=16)
    args=p.parse_args()
    if args.seconds<0 or args.iterations<0 or args.workers<1 or not(args.seconds or args.iterations): p.error('positive duration or iterations and workers required')
    with Site(3,8,delay=.001,retry_every=5,duplicates=True) as site:
        subprocess.run([args.driver,site.url(),str(args.iterations),str(args.seconds),str(args.workers)],check=True)
        snapshot=site.snapshot()
        assert max(snapshot['peak_per_origin'])<=1,snapshot['peak_per_origin']
        assert not any(path.startswith('/leaf/0/') for origin,path in snapshot['counts']), 'robots-blocked request escaped'
        print(json.dumps({'fixture_origins':3,'peak_per_origin':snapshot['peak_per_origin'],'requests':sum(snapshot['counts'].values())}),flush=True)
