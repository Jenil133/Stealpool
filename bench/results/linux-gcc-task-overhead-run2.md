# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [1.27881, 2.38672, 1.33984]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Task overhead (empty fork-join tree)

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.15 | 1.00x | 1.91 | 1.00x | 1.67 |
| 2 | 0.65 | 1.75x | 21.4 | 0.09x | 32.76 |
| 4 | 0.44 | 2.62x | 5.50 | 0.35x | 12.59 |
| 6 | 0.35 | 3.31x | 7.43 | 0.26x | 21.49 |
| 8 | 0.30 | 3.78x | 8.90 | 0.21x | 29.39 |
| 10 | 0.30 | 3.81x | 11.6 | 0.16x | 38.67 |
| 12 | 0.26 | 4.35x | 13.7 | 0.14x | 52.11 |
| 16 | 0.36 | 3.15x | 28.5 | 0.07x | 78.26 |
