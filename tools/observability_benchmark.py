"""Manual Release experiment: identical fixture, measured child CPU/RSS via wait4."""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from backpressure_fixture import FanoutFixture
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests'))
from observability_integration import Client

def run(binary,mode,interval):
    with FanoutFixture(12,120,delay=.015,retry_every=19,duplicates=True) as site:
        command=[binary,'--engine','async','--concurrency','12','--max-per-host','4','--depth','2','--max-pages','3000',
                 '--pending-high-watermark','100','--pending-low-watermark','25','--max-retries','1','--no-retry-jitter','--retry-backoff-ms','100']
        if mode!='disabled': command+=['--dashboard','--dashboard-port','0','--dashboard-linger-ms','0','--metrics-interval-ms',str(interval)]
        command+=[site.url()]
        started=time.perf_counter()
        proc=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        client_thread=None; snapshots=[]
        if mode!='disabled':
            line=proc.stdout.readline(); assert line.startswith('Dashboard: '),line
            port=int(line.strip().rsplit(':',1)[1])
            if mode=='client':
                def consume():
                    client=Client(port)
                    try:
                        while True: snapshots.append(client.snapshot())
                    except (OSError,EOFError): pass
                    finally: client.close()
                client_thread=threading.Thread(target=consume);client_thread.start()
        pid,status,usage=os.wait4(proc.pid,0)
        elapsed=time.perf_counter()-started
        proc.returncode=os.waitstatus_to_exitcode(status)
        if client_thread: client_thread.join()
        out,err=proc.communicate()
        assert proc.returncode==0,(out,err)
        attempts=int(out.split('Pages attempted: ')[1].splitlines()[0])
        return dict(mode=mode,interval_ms=interval if mode!='disabled' else None,elapsed_s=round(elapsed,5),
            attempts=attempts,requests_per_s=round(attempts/elapsed,2),cpu_s=round(usage.ru_utime+usage.ru_stime,5),
            max_rss_bytes=usage.ru_maxrss if sys.platform=='darwin' else usage.ru_maxrss*1024,snapshots=len(snapshots))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',default='./build-release/fluxcrawl');parser.add_argument('--repeats',type=int,default=3)
    args=parser.parse_args()
    # Rotate ordering to reduce order effects. JSONL stdout can be redirected to /tmp.
    cases=[('disabled',500),('no-clients',500),('client',500),('client',100),('client',1000)]
    for repeat in range(args.repeats):
        for mode,interval in cases[repeat%len(cases):]+cases[:repeat%len(cases)]:
            print(json.dumps(dict(repeat=repeat,**run(args.binary,mode,interval))),flush=True)
