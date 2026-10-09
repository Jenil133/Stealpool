# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [6.78613, 5.78271, 3.93848]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=16, tree_cutoff=32, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 28.4 | 1.00x | 28.8 | 1.00x | 1.02 |
| 2 | 14.5 | 1.96x | 28.9 | 1.00x | 2.00 |
| 4 | 7.89 | 3.60x | 32.2 | 0.90x | 4.08 |
| 6 | 8.01 | 3.54x | 44.4 | 0.65x | 5.54 |
| 8 | 7.18 | 3.95x | 51.5 | 0.56x | 7.17 |
| 10 | 7.31 | 3.88x | 49.8 | 0.58x | 6.81 |
| 12 | 7.26 | 3.91x | 47.1 | 0.61x | 6.49 |
| 16 | 7.07 | 4.01x | 46.6 | 0.62x | 6.58 |
