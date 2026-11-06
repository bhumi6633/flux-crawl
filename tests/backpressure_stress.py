"""Opt-in 10k-resource stress with robots, retries, SQLite and a tiny queue cap."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_benchmark import trial
for engine in ('threaded','async'):
    row,summary=trial(Path(sys.argv[1]).resolve(),engine,100,50,True,20,500,100,True)
    assert row['peak_pending']<=100 and row['peak_unfinished']<=100 and row['peak_deferred']>1000
    assert row['buffered_mutations_peak']<4096 and row['queued_batches_peak']<=8
    print(engine,'10,021 logical URLs, 100 retries, robots, SQLite: passed')
    print(summary)
