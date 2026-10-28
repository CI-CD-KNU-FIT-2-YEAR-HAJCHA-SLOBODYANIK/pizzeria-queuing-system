# Theoretical Queueing Model ($M/M/c/m$) & Mathematical Report

**Project Topic**: Pizzeria Queuing System Optimization & Active Ovens Management (Option 4)  
**Role**: Role 3 (Theoretical Modeler & Technical Writer)  
**Operating Window**: 09:00 – 22:00 (780 minutes / 13 hours total, time step $\Delta t = 5\text{ min}$)  
**System Limits**: Active ovens $c \in [1, 10]$, Queue capacity $m = 10$, Maximum system capacity $K = c + m \le 20$.

---

## 1. System Parameters & Shift Profiles

The pizzeria operation exhibits dynamic non-stationary customer demand throughout the 780-minute working day. Under piecewise stationary approximation, the day is partitioned into five operational regimes:

| Shift Name | Clock Interval | Duration ($T$) | Arrival Rate $\lambda$ (orders/hr) | Mean Inter-arrival $1/\lambda$ | Service Rate $\mu$ (per oven/hr) | Mean Bake Time $1/\mu$ | Active Ovens ($c$) | Offered Load $\rho = \lambda/\mu$ | Server Load $\psi = \rho/c$ |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Morning Ramp-Up** | 09:00 – 11:30 | 2.5 h (150 min) | $8.0$ | $7.5\text{ min}$ | $6.0$ | $10\text{ min}$ | $2$ | $1.333$ | $0.667$ |
| **Lunch Rush** | 11:30 – 14:00 | 2.5 h (150 min) | $36.0$ | $1.67\text{ min}$ | $6.0$ | $10\text{ min}$ | $7$ | $6.000$ | $0.857$ |
| **Afternoon Lull** | 14:00 – 17:30 | 3.5 h (210 min) | $16.0$ | $3.75\text{ min}$ | $6.0$ | $10\text{ min}$ | $3$ | $2.667$ | $0.889$ |
| **Dinner Peak** | 17:30 – 20:30 | 3.0 h (180 min) | $48.0$ | $1.25\text{ min}$ | $6.0$ | $10\text{ min}$ | $9$ | $8.000$ | $0.889$ |
| **Evening Wind-Down** | 20:30 – 22:00 | 1.5 h (90 min) | $12.0$ | $5.0\text{ min}$ | $6.0$ | $10\text{ min}$ | $2$ | $2.000$ | $1.000$ |

---

## 2. Markov Chain Formulation & State Transitions

Let $N(t) \in \{0, 1, 2, \dots, c+m\}$ be the number of orders in the pizzeria at time $t$ (both in baking and in the queue).

### 2.1. State Space
$$S = \{0, 1, 2, \dots, c, c+1, \dots, c+m\}$$
- $k \in [0, c]$: All $k$ orders are being actively baked in $k$ ovens; queue is empty ($L_q = 0$).
- $k \in [c+1, c+m]$: All $c$ ovens are busy; $j = k - c$ orders are waiting in the queue.
- $k = c + m$: System is at full capacity. Any subsequent arriving customer is rejected and lost.

### 2.2. Birth and Death Transition Rates

$$\lambda_k = \begin{cases} \lambda, & 0 \le k < c + m \\ 0, & k \ge c + m \end{cases}$$

$$\mu_k = \begin{cases} k \mu, & 1 \le k \le c \\ c \mu, & c < k \le c + m \end{cases}$$

### 2.3. Kolmogorov Balance Equations (Steady State)

$$\frac{d P_k(t)}{dt} = 0 \implies \text{Rate Out of State } k = \text{Rate Into State } k$$

For local balance:
$$\lambda P_{k-1} = \mu_k P_k, \quad \forall k \in \{1, 2, \dots, c+m\}$$

---

## 3. Exact Mathematical Equations Used in the Solver

### 3.1. Dimensionless Load Factors
- **Offered load (Traffic Intensity)**:
  $$\rho = \frac{\lambda}{\mu}$$
- **Channel load factor (Utilization per Server)**:
  $$\psi = \frac{\rho}{c} = \frac{\lambda}{c \mu}$$

### 3.2. State Probabilities ($P_k$)
By recursion through the balance equations:
- **Free Server Regime ($0 \le k \le c$)**:
  $$P_k = \frac{\rho^k}{k!} P_0$$
- **Queueing Regime ($c < k \le c + m$, letting $j = k - c$)**:
  $$P_{c+j} = \frac{\rho^c}{c!} \psi^j P_0$$

### 3.3. Idle Probability ($P_0$)
From total probability normalization $\sum_{k=0}^{c+m} P_k = 1$:
$$P_0 = \left[ \sum_{k=0}^c \frac{\rho^k}{k!} + \frac{\rho^c}{c!} \sum_{j=1}^m \psi^j \right]^{-1}$$

Evaluating the finite geometric series:
$$\boxed{P_0 = \begin{cases} \left[ \sum_{k=0}^c \dfrac{\rho^k}{k!} + \dfrac{\rho^c}{c!} \cdot \dfrac{\psi (1 - \psi^m)}{1 - \psi} \right]^{-1}, & \psi \ne 1 \\[10pt] \left[ \sum_{k=0}^c \dfrac{\rho^k}{k!} + \dfrac{\rho^c}{c!} \cdot m \right]^{-1}, & \psi = 1 \end{cases}}$$

### 3.4. Rejection / Loss Probability ($P_{loss} = P_{reject}$)
Occurs when all $c$ ovens and all $m$ queue slots are full ($k = c + m$):
$$\boxed{P_{loss} = P_{c+m} = \frac{\rho^c}{c!} \psi^m P_0}$$

### 3.5. Throughput Metrics
- **Relative Throughput ($Q$)**:
  $$Q = 1 - P_{loss}$$
- **Absolute Throughput ($A$)**:
  $$A = \lambda \cdot Q = \lambda (1 - P_{loss})$$

### 3.6. Server Utilization ($L_s, \eta$)
- **Average Busy Ovens ($L_s$)**:
  $$L_s = \frac{A}{\mu} = \rho (1 - P_{loss})$$
- **Oven Utilization Efficiency ($\eta$)**:
  $$\eta = \frac{L_s}{c} = \psi (1 - P_{loss})$$

### 3.7. Average Queue Length ($L_q$)
Expected orders waiting in buffer:
$$L_q = \sum_{j=1}^m j \cdot P_{c+j} = \frac{\rho^c}{c!} P_0 \sum_{j=1}^m j \psi^j$$

Evaluating the summation analytically:
$$\boxed{L_q = \begin{cases} \dfrac{\rho^c}{c!} P_0 \cdot \dfrac{\psi \left[ 1 - (m+1)\psi^m + m \psi^{m+1} \right]}{(1 - \psi)^2}, & \psi \ne 1 \\[10pt] \dfrac{\rho^c}{c!} P_0 \cdot \dfrac{m(m+1)}{2}, & \psi = 1 \end{cases}}$$

### 3.8. Waiting Time & Turnaround Time (Little's Law)
- **Average Queue Waiting Time ($W_q$)**:
  $$W_q = \frac{L_q}{A} = \frac{L_q}{\lambda (1 - P_{loss})}$$
- **Average Total Time in System ($W$)**:
  $$W = W_q + \frac{1}{\mu} = \frac{L}{A}, \quad \text{where } L = L_s + L_q$$

---

## 4. Economic Optimization Model

For any operational period $T$ (hours) with active ovens $c$:

1. **Expected Order Flow**:
   $$N_{arr} = \lambda \cdot T, \quad N_{serv} = A \cdot T, \quad N_{lost} = N_{arr} - N_{serv}$$
2. **Gross Revenue**:
   $$R_{gross} = N_{serv} \cdot R_{order} \quad (R_{order} = 250\text{ UAH/pizza})$$
3. **Oven Energy / Operating Cost**:
   $$C_{oven} = T \cdot C_{oven\_hr} \cdot \left[ L_s + (c - L_s) \cdot \alpha_{idle} \right]$$
   - $C_{oven\_hr} = 60\text{ UAH/hour per active oven}$
   - $\alpha_{idle} = 0.5$ (50% power consumption when idling)
4. **Customer Loss Penalty**:
   $$C_{loss} = N_{lost} \cdot C_{penalty} \quad (C_{penalty} = 50\text{ UAH/lost customer})$$
5. **Fixed Overheads**:
   $$C_{fixed} = T \cdot C_{overhead\_hr} \quad (C_{overhead\_hr} = 120\text{ UAH/hr})$$
6. **Net Operating Profit**:
   $$\Pi = R_{gross} - (C_{oven} + C_{loss} + C_{fixed})$$

---

## 5. Daily Theoretical Benchmark Results

Using `analytical_solver.py` for the 13-hour operating day (09:00 – 22:00 / 780 minutes):

| Shift | Interval | Dur | $\lambda$ | $c$ | $\psi$ | $P_0$ | $P_{loss}$ | $A$ (/h) | $L_s$ | $L_q$ | $W_q$ (min) | Net Profit (UAH) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Morning Ramp-Up** | 09:00-11:30 | 2.5h | 8.0 | 2 | 0.667 | 0.2012 | 0.0031 | 7.98 | 1.33 | 0.99 | 7.47 | 4,431.70 UAH |
| **Lunch Rush** | 11:30-14:00 | 2.5h | 36.0 | 7 | 0.857 | 0.0018 | 0.0212 | 35.24 | 5.87 | 1.99 | 3.39 | 20,663.38 UAH |
| **Afternoon Lull** | 14:00-17:30 | 3.5h | 16.0 | 3 | 0.889 | 0.0359 | 0.0349 | 15.44 | 2.57 | 2.86 | 11.10 | 12,408.32 UAH |
| **Dinner Peak** | 17:30-20:30 | 3.0h | 48.0 | 9 | 0.889 | 0.0002 | 0.0272 | 46.69 | 7.78 | 2.23 | 2.86 | 32,953.58 UAH |
| **Evening Wind-Down** | 20:30-22:00 | 1.5h | 12.0 | 2 | 1.000 | 0.0400 | 0.0800 | 11.04 | 1.84 | 4.40 | 23.91 | 3,715.20 UAH |
| **Whole Day Summary** | **09:00-22:00** | **13.0h** | **328 arr.** | **$\bar{c}=4.85$** | **—** | **—** | **2.83%** | **318.7 serv.** | **—** | **2.37** | **5.79 min** | **74,172.17 UAH** |

---

## 6. How to Run the Analytical Model

### Direct CLI Execution
```bash
# Full 780-minute benchmark day report
python analytical_solver.py

# Single-point evaluation: c=5, m=10, lambda=28.0/hr, mu=6.0/hr
python analytical_solver.py --single 5 10 28.0 6.0
```

### Python API Integration (for Participant 2)
```python
from analytical_solver import MMcmQueueingSolver, solve_mmcm

# Instant theoretical solution
metrics = solve_mmcm(c=7, m=10, lambda_rate=36.0, mu_rate=6.0)
print(f"Idle Prob: {metrics.p0:.4f}, Loss Prob: {metrics.p_loss:.4f}, Wq: {metrics.avg_wait_time_queue * 60:.2f} min")
```
