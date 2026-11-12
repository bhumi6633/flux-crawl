"""Separate bounded queue, persistence, exact dedup and restore experiments."""
import argparse
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from backpressure_benchmark import trial as pressure_trial
from persistence_benchmark import trial as persistence_trial

def write(path,rows):
    with path.open('x',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,default=Path('build-release'));p.add_argument('--output',type=Path,default=Path('certification-results/advanced'));p.add_argument('--runs',type=int,default=3);a=p.parse_args()
    if a.runs<1 or a.output.exists():p.error('positive runs and new output directory required')
    os.environ['NO_PROXY']=os.environ['no_proxy']='*' # Fixtures are loopback-only.
    a.output.mkdir(parents=True);build=a.build.resolve();binary=build/'fluxcrawl'
    pressure=[];persistence=[];dedup=[]
    for run in range(a.runs):
        for high,low in [(None,0),(100,50),(20,5)]:
            row,_=pressure_trial(binary,'async',high,low,False,12,30,0,False);row['run']=run+1;pressure.append(row)
        for batch in [None,1,100,500]:
            row=persistence_trial(binary,'async',batch,30,8,2);row['run']=run+1;persistence.append(row)
        for mode,expected in [('memory',100000),('sqlite',100000),('bloom-sqlite',100000),('bloom-sqlite',5000)]:
            with tempfile.TemporaryDirectory() as tmp:
                proc=subprocess.Popen([str(build/'fluxcrawl_dedup_scale'),mode,'100000',str(expected),'0.01',tmp+'/state.db'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                _,status,usage=os.wait4(proc.pid,0);proc.returncode=os.waitstatus_to_exitcode(status)
                out,err=proc.communicate();assert proc.returncode==0,out+err
                for row in csv.DictReader(io.StringIO(out)):
                    row.update(run=run+1,peak_rss_bytes=usage.ru_maxrss if sys.platform=='darwin' else usage.ru_maxrss*1024,cpu_s=usage.ru_utime+usage.ru_stime);dedup.append(row)
        print('advanced repeat',run+1,flush=True)
    write(a.output/'backpressure.csv',pressure);write(a.output/'persistence.csv',persistence);write(a.output/'dedup.csv',dedup)
    for sqlite in [False,True]:
        with tempfile.TemporaryDirectory() as tmp:
            command=[str(build/'fluxcrawl_persistence_scale'),*(['--sqlite'] if sqlite else []),tmp,'1000','10000']
            result=subprocess.run(command,capture_output=True,text=True,check=True,timeout=120)
            (a.output/('sqlite-resume.csv' if sqlite else 'snapshot-resume.csv')).write_text(result.stdout)
