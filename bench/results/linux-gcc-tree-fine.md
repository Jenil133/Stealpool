# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [2.24365, 1.76172, 0.763672]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=16, tree_cutoff=32, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 24.6 | 1.00x | 24.6 | 1.00x | 1.00 |
| 2 | 12.3 | 2.00x | 24.6 | 1.00x | 2.00 |
| 4 | 6.35 | 3.88x | 30.8 | 0.80x | 4.85 |
| 6 | 6.09 | 4.04x | 36.1 | 0.68x | 5.93 |
| 8 | 5.24 | 4.69x | 41.3 | 0.60x | 7.87 |
| 10 | 4.64 | 5.30x | 46.2 | 0.53x | 9.96 |
| 12 | 4.97 | 4.95x | 46.6 | 0.53x | 9.39 |
| 16 | 5.36 | 4.59x | 55.3 | 0.45x | 10.31 |
