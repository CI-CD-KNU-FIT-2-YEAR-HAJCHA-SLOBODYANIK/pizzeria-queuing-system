"""Standalone Analytical M/M/c/m Queueing Theory Solver for Pizzeria Optimization.

Course: Computer Modeling of Processes (University Project, Option 4)
Role: Role 3 (Theoretical Modeler & Technical Writer)
Language: English

System Description:
    - Multi-channel queueing system with finite buffer: M/M/c/m
    - Parallel active ovens: c in [1, 10]
    - Queue capacity: m = 10
    - System capacity: K = c + m (orders in baking + orders waiting)
    - Operating hours: 09:00 - 22:00 (780 minutes / 13 hours total, dt = 5 min)
    - Markovian arrival process: Poisson with arrival rate lambda (orders/hour)
    - Markovian service process: Exponential with service rate mu (pizzas/hour per oven)
"""

from __future__ import annotations
import math
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class QueueMetrics:
    """Performance metrics for an M/M/c/m queueing system."""

    c: int
    m: int
    capacity_K: int
    lambda_rate: float
    mu_rate: float
    rho: float
    psi: float
    p0: float
    p_loss: float
    relative_throughput: float
    absolute_throughput: float
    avg_busy_servers: float
    server_utilization: float
    avg_queue_length: float
    avg_system_entities: float
    avg_wait_time_queue: float
    avg_turnaround_time: float
    state_probabilities: Dict[int, float]

    def to_dict(self) -> Dict[str, float]:
        return {
            "Active Ovens (c)": float(self.c),
            "Queue Capacity (m)": float(self.m),
            "System Capacity (K)": float(self.capacity_K),
            "Arrival Rate (lambda)": self.lambda_rate,
            "Service Rate per Oven (mu)": self.mu_rate,
            "Offered Load (rho)": self.rho,
            "Channel Load (psi)": self.psi,
            "Idle Probability (P0)": self.p0,
            "Loss Probability (P_loss)": self.p_loss,
            "Relative Throughput (Q)": self.relative_throughput,
            "Absolute Throughput (A)": self.absolute_throughput,
            "Average Busy Ovens (Ls)": self.avg_busy_servers,
            "Oven Utilization (eta)": self.server_utilization,
            "Average Queue Length (Lq)": self.avg_queue_length,
            "Average Orders in System (L)": self.avg_system_entities,
            "Average Queue Wait Time (Wq, hrs)": self.avg_wait_time_queue,
            "Average Queue Wait Time (Wq, min)": self.avg_wait_time_queue * 60.0,
            "Average Total System Time (W, hrs)": self.avg_turnaround_time,
            "Average Total System Time (W, min)": self.avg_turnaround_time * 60.0,
        }


class MMcmQueueingSolver:
    """Exact analytical solver for M/M/c/m finite-buffer queueing systems."""

    def __init__(self, c: int, m: int = 10) -> None:
        """Initialize with active ovens c and queue capacity m."""
        if c < 1:
            raise ValueError(f"Active ovens c must be >= 1, received {c}.")
        if m < 0:
            raise ValueError(f"Queue capacity m must be >= 0, received {m}.")
        self.c = int(c)
        self.m = int(m)
        self.capacity_K = self.c + self.m

    def solve(self, lambda_rate: float, mu_rate: float) -> QueueMetrics:
        """Compute exact steady-state queueing metrics.

        Args:
            lambda_rate: Arrival rate (orders per unit time, e.g., per hour).
            mu_rate: Service completion rate per single oven (pizzas per unit time).

        Returns:
            QueueMetrics dataclass containing all theoretical metrics.
        """
        if lambda_rate < 0.0:
            raise ValueError(f"Arrival rate lambda must be >= 0, got {lambda_rate}.")
        if mu_rate <= 0.0:
            raise ValueError(f"Service rate mu must be > 0, got {mu_rate}.")

        # Handle zero arrival edge case
        if math.isclose(lambda_rate, 0.0):
            probs = {0: 1.0}
            for k in range(1, self.capacity_K + 1):
                probs[k] = 0.0
            return QueueMetrics(
                c=self.c,
                m=self.m,
                capacity_K=self.capacity_K,
                lambda_rate=0.0,
                mu_rate=mu_rate,
                rho=0.0,
                psi=0.0,
                p0=1.0,
                p_loss=0.0,
                relative_throughput=1.0,
                absolute_throughput=0.0,
                avg_busy_servers=0.0,
                server_utilization=0.0,
                avg_queue_length=0.0,
                avg_system_entities=0.0,
                avg_wait_time_queue=0.0,
                avg_turnaround_time=1.0 / mu_rate,
                state_probabilities=probs,
            )

        rho = lambda_rate / mu_rate
        psi = rho / float(self.c)

        # Unnormalized state probability terms: term[k] = P_k / P_0
        terms: List[float] = [1.0]

        # 1. Server saturation region: 1 <= k <= c
        current_term = 1.0
        for k in range(1, self.c + 1):
            current_term *= rho / float(k)
            terms.append(current_term)

        # 2. Queueing region: c < k <= c + m
        queue_term = terms[self.c]
        for _ in range(1, self.m + 1):
            queue_term *= psi
            terms.append(queue_term)

        # Normalization condition: sum_{k=0}^{c+m} P_k = 1  =>  P_0 = 1 / sum(terms)
        sum_terms = sum(terms)
        p0 = 1.0 / sum_terms

        state_probs = {k: terms[k] * p0 for k in range(self.capacity_K + 1)}

        # Rejection / loss probability (state c + m)
        p_loss = state_probs[self.capacity_K]

        # Relative and absolute throughput
        relative_throughput = 1.0 - p_loss
        absolute_throughput = lambda_rate * relative_throughput

        # Average busy servers L_s (flow conservation: L_s = A / mu)
        avg_busy_servers = absolute_throughput / mu_rate
        server_utilization = avg_busy_servers / float(self.c)

        # Average queue length L_q = sum_{j=1}^m j * P_{c+j}
        if self.m == 0:
            avg_queue_length = 0.0
        elif math.isclose(psi, 1.0, rel_tol=1e-9, abs_tol=1e-9):
            # Special case psi == 1.0
            term_c = state_probs[self.c]
            avg_queue_length = term_c * (self.m * (self.m + 1) / 2.0)
        else:
            # Closed-form formula:
            term_c = state_probs[self.c]
            num = psi * (1.0 - (self.m + 1.0) * (psi**self.m) + self.m * (psi ** (self.m + 1)))
            den = (1.0 - psi) ** 2
            avg_queue_length = term_c * (num / den)

        # High-precision verification via direct summation
        direct_lq = sum(j * state_probs[self.c + j] for j in range(1, self.m + 1))
        if not math.isfinite(avg_queue_length) or abs(avg_queue_length - direct_lq) > 1e-6:
            avg_queue_length = direct_lq

        # Average orders in system L = L_s + L_q
        avg_system_entities = avg_busy_servers + avg_queue_length

        # Average waiting time in queue W_q by Little's Law: W_q = L_q / A
        if absolute_throughput > 1e-12:
            avg_wait_time_queue = avg_queue_length / absolute_throughput
        else:
            avg_wait_time_queue = 0.0

        # Average total turnaround time in system W = W_q + 1 / mu
        avg_turnaround_time = avg_wait_time_queue + (1.0 / mu_rate)

        return QueueMetrics(
            c=self.c,
            m=self.m,
            capacity_K=self.capacity_K,
            lambda_rate=lambda_rate,
            mu_rate=mu_rate,
            rho=rho,
            psi=psi,
            p0=p0,
            p_loss=p_loss,
            relative_throughput=relative_throughput,
            absolute_throughput=absolute_throughput,
            avg_busy_servers=avg_busy_servers,
            server_utilization=server_utilization,
            avg_queue_length=avg_queue_length,
            avg_system_entities=avg_system_entities,
            avg_wait_time_queue=avg_wait_time_queue,
            avg_turnaround_time=avg_turnaround_time,
            state_probabilities=state_probs,
        )


@dataclass(frozen=True)
class ShiftWindow:
    """Defines an operational shift during the day."""

    name: str
    start_time: str
    end_time: str
    duration_hours: float
    lambda_per_hr: float
    mu_per_hr: float
    active_ovens_c: int
    queue_capacity_m: int = 10


@dataclass(frozen=True)
class FinancialParameters:
    """Pricing and cost structure (all monetary values in UAH)."""

    revenue_per_order: float = 250.0  # Average price per pizza
    oven_cost_per_hour: float = 60.0  # Electricity/wear cost per running oven/hour
    idle_power_ratio: float = 0.5  # Power factor when oven is on but empty (50%)
    lost_order_penalty: float = 50.0  # Customer goodwill penalty per lost order
    fixed_overhead_per_hour: float = 120.0  # Rent, staff, lighting per hour


class PizzeriaOperatingDay:
    """Evaluates the complete 780-minute (09:00 - 22:00) operating day."""

    def __init__(
        self,
        shifts: Optional[List[ShiftWindow]] = None,
        finance: Optional[FinancialParameters] = None,
    ) -> None:
        self.finance = finance or FinancialParameters()
        self.shifts = shifts or [
            ShiftWindow("Morning Ramp-Up", "09:00", "11:30", 2.5, lambda_per_hr=8.0, mu_per_hr=6.0, active_ovens_c=2),
            ShiftWindow("Lunch Rush", "11:30", "14:00", 2.5, lambda_per_hr=36.0, mu_per_hr=6.0, active_ovens_c=7),
            ShiftWindow("Afternoon Lull", "14:00", "17:30", 3.5, lambda_per_hr=16.0, mu_per_hr=6.0, active_ovens_c=3),
            ShiftWindow("Dinner Peak", "17:30", "20:30", 3.0, lambda_per_hr=48.0, mu_per_hr=6.0, active_ovens_c=9),
            ShiftWindow("Evening Wind-Down", "20:30", "22:00", 1.5, lambda_per_hr=12.0, mu_per_hr=6.0, active_ovens_c=2),
        ]

    def evaluate_shifts(self) -> List[Dict[str, float]]:
        """Calculate analytical queue and financial metrics per shift."""
        results = []
        for s in self.shifts:
            solver = MMcmQueueingSolver(c=s.active_ovens_c, m=s.queue_capacity_m)
            qm = solver.solve(s.lambda_per_hr, s.mu_per_hr)

            # Order volumes
            arrivals = s.lambda_per_hr * s.duration_hours
            served = qm.absolute_throughput * s.duration_hours
            lost = arrivals - served

            # Financial metrics
            gross_revenue = served * self.finance.revenue_per_order
            busy_cost = qm.avg_busy_servers * self.finance.oven_cost_per_hour * s.duration_hours
            idle_ovens = max(0.0, float(s.active_ovens_c) - qm.avg_busy_servers)
            idle_cost = idle_ovens * (self.finance.oven_cost_per_hour * self.finance.idle_power_ratio) * s.duration_hours
            oven_cost = busy_cost + idle_cost
            lost_penalty = lost * self.finance.lost_order_penalty
            overhead = self.finance.fixed_overhead_per_hour * s.duration_hours
            net_profit = gross_revenue - (oven_cost + lost_penalty + overhead)

            results.append({
                "Shift": s.name,
                "Time Window": f"{s.start_time}-{s.end_time}",
                "Duration (h)": s.duration_hours,
                "Arrival lambda (/h)": s.lambda_per_hr,
                "Active Ovens (c)": s.active_ovens_c,
                "Offered Load rho": qm.rho,
                "Oven Load psi": qm.psi,
                "Idle Prob P0": qm.p0,
                "Loss Prob P_loss": qm.p_loss,
                "Abs Throughput A (/h)": qm.absolute_throughput,
                "Avg Busy Ovens Ls": qm.avg_busy_servers,
                "Oven Utilization eta": qm.server_utilization,
                "Avg Queue Lq": qm.avg_queue_length,
                "Avg Wait Wq (min)": qm.avg_wait_time_queue * 60.0,
                "Incoming Orders": arrivals,
                "Served Orders": served,
                "Lost Orders": lost,
                "Gross Revenue (UAH)": gross_revenue,
                "Oven Cost (UAH)": oven_cost,
                "Net Profit (UAH)": net_profit,
            })
        return results

    def print_daily_report(self) -> None:
        """Print a formatted report of shift and whole-day metrics."""
        results = self.evaluate_shifts()
        print("=" * 105)
        print("PIZZERIA 780-MINUTE OPERATING DAY ANALYTICAL EVALUATION (09:00 - 22:00)")
        print("=" * 105)
        header = f"{'Shift':<18} {'Window':<12} {'Dur':<5} {'c':<3} {'lambda':<7} {'P0':<7} {'P_loss':<8} {'A(/h)':<7} {'Ls':<5} {'Lq':<5} {'Wq(m)':<6} {'Net Profit':<12}"
        print(header)
        print("-" * 105)

        total_hours = sum(r["Duration (h)"] for r in results)
        total_arrivals = sum(r["Incoming Orders"] for r in results)
        total_served = sum(r["Served Orders"] for r in results)
        total_lost = sum(r["Lost Orders"] for r in results)
        total_rev = sum(r["Gross Revenue (UAH)"] for r in results)
        total_oven_cost = sum(r["Oven Cost (UAH)"] for r in results)
        total_profit = sum(r["Net Profit (UAH)"] for r in results)
        weighted_lq = sum(r["Avg Queue Lq"] * r["Duration (h)"] for r in results) / total_hours
        weighted_wq = sum(r["Avg Wait Wq (min)"] * r["Served Orders"] for r in results) / total_served

        for r in results:
            print(
                f"{r['Shift']:<18} "
                f"{r['Time Window']:<12} "
                f"{r['Duration (h)']:<5.1f} "
                f"{r['Active Ovens (c)']:<3} "
                f"{r['Arrival lambda (/h)']:<7.1f} "
                f"{r['Idle Prob P0']:<7.4f} "
                f"{r['Loss Prob P_loss']:<8.4f} "
                f"{r['Abs Throughput A (/h)']:<7.2f} "
                f"{r['Avg Busy Ovens Ls']:<5.2f} "
                f"{r['Avg Queue Lq']:<5.2f} "
                f"{r['Avg Wait Wq (min)']:<6.2f} "
                f"{r['Net Profit (UAH)']:>10.2f} UAH"
            )
        print("=" * 105)
        print("DAILY TOTALS & WEIGHTED AVERAGES:")
        print(f"  Operating Time              : {total_hours:.1f} hours (780 minutes)")
        print(f"  Total Incoming Orders       : {total_arrivals:.1f}")
        print(f"  Total Served Orders         : {total_served:.1f}")
        print(f"  Total Rejected / Lost Orders: {total_lost:.1f}")
        print(f"  Overall System Loss Rate    : {(total_lost / total_arrivals) * 100:.2f}%")
        print(f"  Time-Weighted Avg Queue (Lq): {weighted_lq:.3f} orders")
        print(f"  Customer-Weighted Wait (Wq) : {weighted_wq:.2f} minutes")
        print(f"  Total Gross Revenue         : {total_rev:,.2f} UAH")
        print(f"  Total Oven Energy Cost      : {total_oven_cost:,.2f} UAH")
        print(f"  Daily Net Operating Profit  : {total_profit:,.2f} UAH")
        print("=" * 105)


def solve_mmcm(c: int, m: int, lambda_rate: float, mu_rate: float) -> QueueMetrics:
    """Convenience function to solve an M/M/c/m queueing system."""
    solver = MMcmQueueingSolver(c=c, m=m)
    return solver.solve(lambda_rate=lambda_rate, mu_rate=mu_rate)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--single":
        c_in = int(sys.argv[2]) if len(sys.argv) > 2 else 4
        m_in = int(sys.argv[3]) if len(sys.argv) > 3 else 10
        lam_in = float(sys.argv[4]) if len(sys.argv) > 4 else 24.0
        mu_in = float(sys.argv[5]) if len(sys.argv) > 5 else 6.0
        res = solve_mmcm(c=c_in, m=m_in, lambda_rate=lam_in, mu_rate=mu_in)
        print(f"M/M/{c_in}/{m_in} Queueing Evaluation:")
        for k, v in res.to_dict().items():
            print(f"  {k:<35}: {v:12.4f}")
    else:
        day = PizzeriaOperatingDay()
        day.print_daily_report()
