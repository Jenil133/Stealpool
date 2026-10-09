# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [3.04541, 2.84229, 2.55029]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Parallel merge sort

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 316 | 1.00x | 316 | 1.00x | 1.00 |
| 2 | 160 | 1.97x | 160 | 1.97x | 1.00 |
| 4 | 81.1 | 3.90x | 81.3 | 3.89x | 1.00 |
| 6 | 59.8 | 5.28x | 62.3 | 5.07x | 1.04 |
| 8 | 48.2 | 6.55x | 51.7 | 6.11x | 1.07 |
| 10 | 40.9 | 7.72x | 44.8 | 7.05x | 1.10 |
| 12 | 42.5 | 7.43x | 44.9 | 7.03x | 1.06 |
| 16 | 43.0 | 7.34x | 46.0 | 6.87x | 1.07 |
