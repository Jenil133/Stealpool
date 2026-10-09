# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [3.52881, 3.45947, 2.00977]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 121 | 1.00x | 122 | 1.00x | 1.00 |
| 2 | 61.4 | 1.98x | 62.0 | 1.96x | 1.01 |
| 4 | 33.8 | 3.59x | 31.2 | 3.90x | 0.92 |
| 6 | 27.7 | 4.37x | 26.2 | 4.65x | 0.94 |
| 8 | 22.3 | 5.43x | 22.8 | 5.35x | 1.02 |
| 10 | 20.6 | 5.88x | 20.4 | 5.95x | 0.99 |
| 12 | 19.9 | 6.09x | 20.5 | 5.95x | 1.03 |
| 16 | 19.7 | 6.16x | 20.5 | 5.93x | 1.04 |
