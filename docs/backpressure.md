# Step 16: bounded pending work

## Baseline before admission changes

Release, deterministic graph: seed →20 branch pages →500 unique leaves each,
three loopback origins, depth 2. Sequential process stopped at increasing attempt
budgets, before fetching leaves. Pending grows monotonically during this phase,
so the final 10,000 pending items are also its peak. External `/usr/bin/time -l`
measured process peak RSS (Darwin bytes). Separate runs, warm local fixture:

| Attempts | Pending | Seen | Peak RSS bytes |
| ---: | ---: | ---: | ---: |
| 1 | 20 | 21 | 6,864,896 |
| 6 | 2,515 | 2,521 | 9,011,200 |
| 11 | 5,010 | 5,021 | 10,387,456 |
| 16 | 7,505 | 7,521 | 12,288,000 |
| 21 | 10,000 | 10,021 | 13,107,200 |

The full pre-change suite passed before implementation (record below once done).
`tools/backpressure_fixture.py` defines the graph. Raw CSV is ignored in results/.
