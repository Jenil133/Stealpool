# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [6.12061, 6.17188, 4.33447]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Task overhead (empty fork-join tree)

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.87 | 1.00x | 2.52 | 1.00x | 1.35 |
| 2 | 1.06 | 1.77x | 5.71 | 0.44x | 5.41 |
| 4 | 0.76 | 2.45x | 9.73 | 0.26x | 12.76 |
| 6 | 0.78 | 2.39x | 13.4 | 0.19x | 17.12 |
| 8 | 0.77 | 2.41x | 13.7 | 0.18x | 17.66 |
| 10 | 0.78 | 2.41x | 13.4 | 0.19x | 17.21 |
| 12 | 0.80 | 2.34x | 13.0 | 0.19x | 16.25 |
| 16 | 0.88 | 2.13x | 13.2 | 0.19x | 15.01 |
