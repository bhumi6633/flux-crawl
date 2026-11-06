"""Fix async engine, queue bounds and SQLite durability; vary exact dedup strategy."""
import argparse,csv,statistics
from pathlib import Path
from backpressure_benchmark import trial

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path('build-release/fluxcrawl'));p.add_argument('--runs',type=int,default=3);p.add_argument('--warmups',type=int,default=1);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 if a.output.exists() or a.runs<1 or a.warmups<0:p.error('fresh output, positive runs and nonnegative warmups required')
 rows=[]
 for run in range(-a.warmups,a.runs):
  for mode in ('memory','sqlite','bloom-sqlite'):
   row,_=trial(a.binary.resolve(),'async',1000,750,True,20,500,0,False,dedup=mode,bloom_expected=10021,bloom_fp=.01)
   if run>=0:row['run']=run+1;rows.append(row);print(mode,'run',run+1,'elapsed_ms',round(row['elapsed_ms'],3),'RSS',row['peak_rss_bytes'],'DB checks',row['exact_db_queries'],flush=True)
 with a.output.open('x',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 for mode in ('memory','sqlite','bloom-sqlite'):
  r=[v for v in rows if v['dedup_backend']==mode]
  print(mode,{k:statistics.median(v[k] for v in r) for k in ('elapsed_ms','requests_per_second','peak_pending','peak_rss_bytes','dedup_lookup_ms','dedup_insert_ms','exact_db_queries','bloom_false_positives','bloom_definite_misses','bloom_queries','bloom_bytes','seen_cache_peak','commit_ms')})
if __name__=='__main__':main()
