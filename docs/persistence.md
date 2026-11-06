# Persistent storage experiments and design

## Step 14 scaling baseline, before SQLite implementation

Release measurement on the local development machine, three samples per size,
100 unfinished resources, remaining URLs terminally completed, no network traffic.
The existing writer includes validation, serialization, temporary write, flush,
POSIX file fsync, read-back validation and rename. Loads follow writes and thus
exercise warm filesystem caches; these are not cold-disk measurements.

| Seen URLs | Snapshot bytes | Median whole write ms | Median load ms |
| ---: | ---: | ---: | ---: |
| 1,000 | 38,801 | 2.670 | 2.152 |
| 10,000 | 344,907 | 17.794 | 12.141 |
| 100,000 | 3,495,013 | 191.673 | 133.087 |
| 500,000 | 17,895,116 | 1336.706 | 820.739 |

Command (executed before introducing SQLite):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target fluxcrawl_persistence_scale -j 4
./build-release/fluxcrawl_persistence_scale results/step15-baseline \
  1000 10000 100000 500000 > results/step15-snapshot-baseline.csv
```

Every generated/read state was validated. The synthetic URL-construction time is
excluded. Raw CSVs remain ignored in `results/`. This establishes whole-state cost;
future incremental commits are different operations and must not be presented as
like-for-like replacements of full snapshot writes.
