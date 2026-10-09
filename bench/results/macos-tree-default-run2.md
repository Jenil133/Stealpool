# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [4.80762, 6.05566, 4.95166]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 123 | 1.00x | 125 | 1.00x | 1.01 |
| 2 | 66.3 | 1.86x | 65.9 | 1.89x | 0.99 |
| 4 | 34.0 | 3.62x | 37.2 | 3.35x | 1.09 |
| 6 | 28.5 | 4.33x | 29.1 | 4.27x | 1.02 |
| 8 | 24.6 | 5.00x | 25.9 | 4.81x | 1.05 |
| 10 | 22.0 | 5.60x | 22.8 | 5.47x | 1.04 |
| 12 | 22.4 | 5.50x | 23.7 | 5.26x | 1.06 |
| 16 | 22.8 | 5.41x | 23.7 | 5.25x | 1.04 |
