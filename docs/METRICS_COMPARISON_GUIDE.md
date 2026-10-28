# Metrics Comparison Guide & Validation Protocol

**Target Audience**: Participant 2 (Simulation Modeler & Data Analyst) & Team  
**Author**: Role 3 (Theoretical Modeler & Technical Writer)  
**Purpose**: Direct reference for validating discrete-event simulation results against the theoretical $M/M/c/m$ queueing model.

---

## 1. Metric Mapping Specification Table

Use this reference table to map simulation accumulators and statistical estimators directly to the theoretical analytical equations:

| Analytical Metric | Mathematical Symbol | Formula / Definition | Simulation Counter / Estimator | Units | Acceptable Error Tolerance ($\epsilon$) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **System Idle Probability** | $P_0$ | Equation in [THEORETICAL_MODEL.md](file:///docs/THEORETICAL_MODEL.md#L73) | $\frac{\text{Total Time All Ovens Empty}}{\text{Total Simulation Time}}$ | Dimensionless $[0, 1]$ | $\pm 5\%$ relative |
| **Loss / Rejection Probability** | $P_{reject}$ | $P_{c+m} = \frac{\rho^c}{c!} \psi^m P_0$ | $\frac{N_{rejected}}{N_{arrived}}$ | Dimensionless $[0, 1]$ | $\pm 5\%$ relative (or $\pm 0.005$ abs) |
| **Relative Throughput** | $Q$ | $1 - P_{reject}$ | $\frac{N_{served}}{N_{arrived}}$ | Dimensionless $[0, 1]$ | $\pm 1\%$ relative |
| **Absolute Throughput** | $A$ | $\lambda \cdot (1 - P_{reject})$ | $\frac{N_{served}}{T_{sim}}$ | orders / hour (or /min) | $\pm 3\%$ relative |
| **Average Busy Ovens** | $L_s$ | $\frac{A}{\mu} = \rho (1 - P_{reject})$ | $\frac{1}{T_{sim}} \int_0^{T_{sim}} c_{busy}(t) dt$ | active ovens $[0, c]$ | $\pm 3\%$ relative |
| **Oven Utilization** | $\eta$ | $\frac{L_s}{c}$ | $\frac{L_s^{sim}}{c}$ | Dimensionless $[0, 1]$ | $\pm 3\%$ relative |
| **Average Queue Length** | $L_q$ | $\sum_{j=1}^m j \cdot P_{c+j}$ | $\frac{1}{T_{sim}} \int_0^{T_{sim}} N_{queue}(t) dt$ | waiting orders | $\pm 5\%$ relative |
| **Average Orders in System** | $L$ | $L_s + L_q$ | $\frac{1}{T_{sim}} \int_0^{T_{sim}} N_{system}(t) dt$ | orders | $\pm 5\%$ relative |
| **Average Queue Wait Time** | $W_q$ | $\frac{L_q}{A}$ (Little's Law) | $\frac{1}{N_{served}} \sum_{i=1}^{N_{served}} (t_{bake\_start, i} - t_{arrive, i})$ | minutes (or hours) | $\pm 5\%$ relative |
| **Average Total Turnaround** | $W$ | $W_q + \frac{1}{\mu}$ | $\frac{1}{N_{served}} \sum_{i=1}^{N_{served}} (t_{finish, i} - t_{arrive, i})$ | minutes (or hours) | $\pm 5\%$ relative |
| **Gross Revenue** | $R$ | $A \cdot T \cdot R_{order}$ | $\sum_{i=1}^{N_{served}} \text{Price}_i$ | UAH | $\pm 3\%$ relative |
| **Oven Operating Cost** | $C_{ovens}$ | $T \cdot C_{hr} \cdot [L_s + (c - L_s)\alpha]$ | Integrated electricity/fuel counters | UAH | $\pm 3\%$ relative |
| **Net Operating Profit** | $\Pi$ | $R - C_{ovens} - C_{lost} - C_{fixed}$ | $R_{sim} - C_{total, sim}$ | UAH | $\pm 5\%$ relative |

---

## 2. Theoretical Benchmark Table for Option 4 Schedule

These values were computed using `analytical.schedule.PizzeriaDaySchedule` with service rate $\mu = 6.0\text{ orders/hr}$ (10-minute mean bake time per oven), buffer $m = 10$, revenue $= 250\text{ UAH/order}$, and oven cost $= 60\text{ UAH/hr}$:

| Shift Name | Interval | Duration | $\lambda$ | $c$ | $P_0$ | $P_{reject}$ | $A$ (orders/hr) | $L_s$ | $L_q$ | $W_q$ (min) | Net Profit (UAH) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Morning Ramp-Up** | 09:00 – 11:30 | 2.5 h | 8.0 | 2 | 0.2012 | 0.0031 | 7.975 | 1.329 | 0.993 | 7.47 | 4,431.70 |
| **Lunch Rush** | 11:30 – 14:00 | 2.5 h | 36.0 | 7 | 0.0018 | 0.0212 | 35.238 | 5.873 | 1.993 | 3.39 | 20,663.38 |
| **Afternoon Lull** | 14:00 – 17:30 | 3.5 h | 16.0 | 3 | 0.0359 | 0.0349 | 15.441 | 2.574 | 2.856 | 11.10 | 12,408.32 |
| **Dinner Peak** | 17:30 – 20:30 | 3.0 h | 48.0 | 9 | 0.0002 | 0.0272 | 46.693 | 7.782 | 2.227 | 2.86 | 32,953.58 |
| **Evening Closing** | 20:30 – 22:00 | 1.5 h | 12.0 | 2 | 0.0400 | 0.0800 | 11.040 | 1.840 | 4.400 | 23.91 | 3,715.20 |
| **Whole Day Total** | **09:00 – 22:00** | **13.0 h** | **328.0 arr.** | **Avg 4.85** | — | **0.0284** | **318.7 serv.** | **—** | **2.37** | **5.79** | **74,172.18** |

---

## 3. How to Import and Compare in Python

Participant 2 can quickly run automated comparison scripts in Python:

```python
from analytical import solve_mmcm, evaluate_economics, EconomicConfig

# 1. Compute analytical baseline for a specific shift
analytical = solve_mmcm(c=7, m=10, lambda_rate=36.0, mu_rate=6.0)

# 2. Get your simulation metrics from your simulation engine run
sim_loss_rate = sim_results.lost_orders / sim_results.total_arrivals
sim_avg_wait_min = sim_results.mean_queue_wait_minutes

# 3. Print validation report
print(f"Loss Rate: Theory = {analytical.p_reject:.4f} vs Sim = {sim_loss_rate:.4f} (Diff: {abs(analytical.p_reject - sim_loss_rate):.4f})")
print(f"Wait Time: Theory = {analytical.avg_wait_time_queue * 60:.2f}m vs Sim = {sim_avg_wait_min:.2f}m")
```

---

## 4. How to Include in C++ Simulation Engine

Participant 1 or 2 can directly include the analytical solver in `simulation_engine/`:

```cpp
#include "analytical_solver.hpp"
#include <iostream>

void validateAgainstTheoreticalModel() {
    queueing_theory::AnalyticalMMcmSolver solver(7, 10);
    auto theory = solver.solve(36.0, 6.0); // lambda = 36/hr, mu = 6/hr

    std::cout << "Theoretical P_reject: " << theory.p_reject << "\n";
    std::cout << "Theoretical Avg Queue (L_q): " << theory.avg_queue_length << "\n";
    std::cout << "Theoretical Avg Wait (W_q): " << theory.avg_wait_time_queue * 60.0 << " min\n";
}
```
