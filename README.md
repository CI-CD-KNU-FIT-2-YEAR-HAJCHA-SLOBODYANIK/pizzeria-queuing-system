# Pizzeria Queuing System Optimization & Active Ovens Management

**Course**: Computer Modeling of Processes (3rd Year, Semester 1)  
**Topic**: Option 4 – Discrete-event & Analytical Queueing Modeling of a Pizzeria Baking Line with Dynamic Oven Management ($M/M/c/m$).  
**Operating Hours**: 09:00 – 22:00 (780 minutes / 13 hours total, time step $\Delta t = 5\text{ min}$, queue capacity $m = 10$, total oven limit $\le 10$).

---

## Team Roles & Structure

- **Role 1 (Simulation Engine Developer)**: C++ discrete-event core simulation engine (`simulation_engine/`).
- **Role 2 (Simulation Modeler & Data Analyst)**: Experimentation, parameter calibration, comparative graphs, and Monte Carlo runs.
- **Role 3 (Theoretical Modeler & Technical Writer)**: Analytical $M/M/c/m$ queueing theory solver, steady-state mathematical formulations, financial model, and documentation.

---

## Analytical Queueing Module ($M/M/c/m$)

The analytical module calculates exact steady-state state probabilities and performance metrics for multi-server queueing systems with finite buffers ($c \le 10$ active ovens, queue capacity $m = 10$):
- **Idle Probability ($P_0$)** and State Distribution ($P_k$)
- **Loss / Rejection Probability ($P_{reject} = P_{c+m}$)**
- **Relative Throughput ($Q$)** and **Absolute Throughput ($A$)**
- **Average Busy Ovens ($L_s$)** and **Oven Utilization ($\eta$)**
- **Average Queue Length ($L_q$)** and **Queue Wait Time ($W_q$)**
- **Economic Evaluation**: Gross revenue, oven operational costs (active vs. idle power), lost customer penalties, and net profit.

### Quick Start (Python)

Run the full 780-minute operating day analytical evaluation:
```bash
python -m analytical.cli --benchmark
```

Run a single-shift evaluation:
```bash
python -m analytical.cli --single --c 5 --m 10 --lambda-rate 30.0 --mu-rate 6.0
```

Find the optimal number of active ovens $c^*$ for a given traffic intensity:
```bash
python -m analytical.cli --optimize --lambda-rate 36.0 --mu-rate 6.0
```

Export theoretical benchmark data to CSV:
```bash
python -m analytical.cli --export-csv "analytical_benchmark.csv"
```

### Quick Start (C++)

Role 1 and 2 can also directly include the analytical solver header in C++:
```cpp
#include "analytical_solver.hpp"

queueing_theory::AnalyticalMMcmSolver solver(7, 10);
auto metrics = solver.solve(36.0, 6.0); // lambda = 36.0, mu = 6.0
```

---

## Documentation

- [Mathematical Model Specification](docs/THEORETICAL_MODEL.md): Complete derivations, Kolmogorov state equations, closed-form formulas, and Mermaid Markov diagrams.
- [Metrics Comparison & Validation Guide](docs/METRICS_COMPARISON_GUIDE.md): Mapping table between analytical metrics and simulation estimators for Participant 2.

---

## Testing

Run unit tests:
```bash
python -m pytest tests/ -v
```
