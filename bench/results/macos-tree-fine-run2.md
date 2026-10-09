# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [4.97559, 6.05713, 4.77295]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=16, tree_cutoff=32, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 28.5 | 1.00x | 29.3 | 1.00x | 1.03 |
| 2 | 14.4 | 1.98x | 29.3 | 1.00x | 2.04 |
| 4 | 9.13 | 3.12x | 32.6 | 0.90x | 3.57 |
| 6 | 8.14 | 3.51x | 47.4 | 0.62x | 5.83 |
| 8 | 7.95 | 3.59x | 51.2 | 0.57x | 6.44 |
| 10 | 7.73 | 3.69x | 50.4 | 0.58x | 6.52 |
| 12 | 6.98 | 4.09x | 47.0 | 0.62x | 6.73 |
| 16 | 7.49 | 3.81x | 47.2 | 0.62x | 6.31 |
