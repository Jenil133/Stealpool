# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler GNU 13.3.0, build Release, load avg [1.99414, 2.646, 1.56738]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=16, tree_cutoff=32, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 24.3 | 1.00x | 25.4 | 1.00x | 1.04 |
| 2 | 12.3 | 1.98x | 22.4 | 1.13x | 1.83 |
| 4 | 7.19 | 3.38x | 27.5 | 0.92x | 3.83 |
| 6 | 6.08 | 4.00x | 38.0 | 0.67x | 6.25 |
| 8 | 4.85 | 5.01x | 38.1 | 0.67x | 7.85 |
| 10 | 4.75 | 5.12x | 46.2 | 0.55x | 9.72 |
| 12 | 4.76 | 5.11x | 43.0 | 0.59x | 9.05 |
| 16 | 4.65 | 5.23x | 50.4 | 0.50x | 10.83 |
