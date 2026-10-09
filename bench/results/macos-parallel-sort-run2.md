# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [4.51123, 6.0708, 5.1167]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Parallel merge sort

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 324 | 1.00x | 319 | 1.00x | 0.98 |
| 2 | 163 | 1.99x | 175 | 1.82x | 1.08 |
| 4 | 83.7 | 3.87x | 85.5 | 3.73x | 1.02 |
| 6 | 65.8 | 4.93x | 68.1 | 4.69x | 1.04 |
| 8 | 54.7 | 5.93x | 54.9 | 5.81x | 1.00 |
| 10 | 46.3 | 6.99x | 50.6 | 6.31x | 1.09 |
| 12 | 48.5 | 6.69x | 55.3 | 5.77x | 1.14 |
| 16 | 46.0 | 7.05x | 50.0 | 6.38x | 1.09 |
