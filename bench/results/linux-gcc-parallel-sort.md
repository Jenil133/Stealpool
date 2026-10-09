# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [0.529297, 0.209473, 0.0761719]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Parallel merge sort

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 561 | 1.00x | 583 | 1.00x | 1.04 |
| 2 | 286 | 1.96x | 285 | 2.05x | 0.99 |
| 4 | 157 | 3.58x | 163 | 3.57x | 1.04 |
| 6 | 110 | 5.11x | 113 | 5.18x | 1.03 |
| 8 | 87.5 | 6.41x | 90.1 | 6.48x | 1.03 |
| 10 | 80.9 | 6.94x | 80.1 | 7.29x | 0.99 |
| 12 | 74.1 | 7.58x | 81.0 | 7.20x | 1.09 |
| 16 | 77.0 | 7.29x | 75.4 | 7.74x | 0.98 |
