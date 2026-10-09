# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [4.0293, 5.91699, 4.53711]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Task overhead (empty fork-join tree)

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.82 | 1.00x | 2.38 | 1.00x | 1.30 |
| 2 | 1.05 | 1.74x | 5.62 | 0.42x | 5.35 |
| 4 | 0.67 | 2.73x | 8.10 | 0.29x | 12.11 |
| 6 | 0.61 | 3.00x | 12.8 | 0.19x | 21.10 |
| 8 | 0.65 | 2.79x | 13.4 | 0.18x | 20.51 |
| 10 | 0.59 | 3.08x | 12.8 | 0.19x | 21.54 |
| 12 | 0.72 | 2.52x | 12.6 | 0.19x | 17.47 |
| 16 | 0.74 | 2.48x | 12.9 | 0.18x | 17.58 |
