# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [2.81494, 3.61279, 2.24609]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Parallel merge sort

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 559 | 1.00x | 567 | 1.00x | 1.01 |
| 2 | 290 | 1.93x | 297 | 1.90x | 1.03 |
| 4 | 151 | 3.70x | 154 | 3.68x | 1.02 |
| 6 | 111 | 5.03x | 107 | 5.28x | 0.97 |
| 8 | 98.3 | 5.68x | 95.8 | 5.92x | 0.97 |
| 10 | 74.9 | 7.45x | 84.9 | 6.67x | 1.13 |
| 12 | 72.7 | 7.68x | 79.7 | 7.11x | 1.10 |
| 16 | 86.4 | 6.47x | 83.1 | 6.82x | 0.96 |
