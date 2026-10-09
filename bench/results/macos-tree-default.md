# Stealpool scaling results

Statistic: median wall-clock time per run.

- 10 CPUs, compiler AppleClang 21.0.0.21000101, build Release, load avg [3.91504, 3.74463, 2.97607]; workload: sort_size=10000000, tree_nodes=2097152, tree_work=64, tree_cutoff=256, spawn_depth=16

## Unbalanced tree reduction

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 121 | 1.00x | 132 | 1.00x | 1.09 |
| 2 | 64.7 | 1.88x | 68.4 | 1.93x | 1.06 |
| 4 | 39.0 | 3.11x | 32.9 | 4.02x | 0.84 |
| 6 | 27.6 | 4.39x | 33.2 | 3.99x | 1.20 |
| 8 | 23.0 | 5.27x | 23.2 | 5.69x | 1.01 |
| 10 | 20.4 | 5.94x | 21.0 | 6.30x | 1.03 |
| 12 | 20.9 | 5.81x | 21.8 | 6.08x | 1.04 |
| 16 | 20.3 | 5.99x | 20.7 | 6.40x | 1.02 |
