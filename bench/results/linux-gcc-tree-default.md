# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [2.36279, 1.23926, 0.478516]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 132 | 1.00x | 124 | 1.00x | 0.94 |
| 2 | 62.5 | 2.12x | 62.5 | 1.99x | 1.00 |
| 4 | 33.9 | 3.90x | 37.4 | 3.32x | 1.10 |
| 6 | 27.8 | 4.76x | 29.2 | 4.25x | 1.05 |
| 8 | 25.6 | 5.16x | 25.3 | 4.91x | 0.99 |
| 10 | 23.1 | 5.73x | 23.9 | 5.19x | 1.04 |
| 12 | 22.4 | 5.91x | 22.0 | 5.65x | 0.98 |
| 16 | 22.4 | 5.89x | 23.7 | 5.25x | 1.06 |
