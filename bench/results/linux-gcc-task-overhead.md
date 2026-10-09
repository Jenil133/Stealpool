# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [2.92529, 2.52979, 1.17529]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Task overhead (empty fork-join tree)

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.16 | 1.00x | 1.90 | 1.00x | 1.64 |
| 2 | 0.66 | 1.75x | 20.5 | 0.09x | 30.97 |
| 4 | 0.48 | 2.41x | 5.84 | 0.33x | 12.16 |
| 6 | 0.35 | 3.32x | 7.43 | 0.26x | 21.28 |
| 8 | 0.38 | 3.08x | 9.16 | 0.21x | 24.39 |
| 10 | 0.34 | 3.40x | 11.8 | 0.16x | 34.76 |
| 12 | 0.34 | 3.44x | 15.9 | 0.12x | 47.26 |
| 16 | 0.33 | 3.51x | 27.8 | 0.07x | 84.22 |
