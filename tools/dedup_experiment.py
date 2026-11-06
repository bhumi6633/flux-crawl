"""Release synthetic exact membership/RSS and Bloom probability/saturation sweep."""
import argparse,csv,io,re,statistics,subprocess,tempfile
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path('build-release/fluxcrawl_dedup_scale'));p.add_argument('--output',type=Path,required=True);p.add_argument('--runs',type=int,default=3);a=p.parse_args()
 if a.output.exists() or a.runs<1:p.error('fresh output and positive runs required')
 configurations=[(mode,n,n,.01,'scale') for n in (100000,500000,1000000) for mode in ('memory','memory-reserve','sqlite','bloom-sqlite')]
 configurations += [('bloom-sqlite',100000,100000,p,'fp-sweep') for p in (.1,.01,.001)]
 configurations += [('bloom-sqlite',n,100000,.01,'saturation') for n in (100000,200000,500000)]
 rows=[]
 with tempfile.TemporaryDirectory() as tmp:
  for mode,n,expected,fp,experiment in configurations:
   for run in range(a.runs):
    command=[str(a.binary.resolve()),mode,str(n),str(expected),str(fp),str(Path(tmp)/'state.db')]
    result=subprocess.run(['/usr/bin/time','-l',*command],capture_output=True,text=True,timeout=240)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    rss=re.search(r'(\d+)\s+maximum resident set size',result.stderr)
    assert rss,result.stderr
    for row in csv.DictReader(io.StringIO(result.stdout)):
     row.update(experiment=experiment,run=run+1,peak_rss_bytes=int(rss[1]));rows.append(row)
   print(experiment,mode,n,'capacity',expected,'p',fp,flush=True)
 with a.output.open('x',newline='') as f:
  writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
 print('Validated rows:',len(rows))
if __name__=='__main__':main()
