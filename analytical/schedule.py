"""Dynamic Schedule and Multi-Window Evaluation Module for Pizzeria Queuing System.

Handles operational day simulation (09:00 - 22:00, 780 minutes / 13 hours),
time-window parameter variation (arrival rates, active ovens, service rates),
and optimal active oven allocation c*(t).
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple
import pandas as pd

from .solver import MMcmModel, QueueingMetrics
from .economics import EconomicConfig, EconomicMetrics, evaluate_economics


@dataclass
class TimeWindow:
    """Operational time block with specific queueing parameters.

    Attributes:
        name: Label for this shift or period (e.g., 'Lunch Rush').
        start_min: Start time in minutes from midnight (e.g., 540 for 09:00).
        end_min: End time in minutes from midnight (e.g., 720 for 12:00).
        lambda_per_hr: Incoming arrival rate in orders per hour.
        mu_per_hr: Service rate per oven in orders per hour (e.g., 6.0 = 10 min bake time).
        ovens_c: Number of active ovens operating in this window (1 <= c <= 10).
        queue_m: Max queue capacity (defaults to 10).
    """

    name: str
    start_min: int
    end_min: int
    lambda_per_hr: float
    mu_per_hr: float
    ovens_c: int
    queue_m: int = 10

    @property
    def duration_minutes(self) -> int:
        return self.end_min - self.start_min

    @property
    def duration_hours(self) -> float:
        return self.duration_minutes / 60.0

    @property
    def start_clock(self) -> str:
        return f"{self.start_min // 60:02d}:{self.start_min % 60:02d}"

    @property
    def end_clock(self) -> str:
        return f"{self.end_min // 60:02d}:{self.end_min % 60:02d}"


def optimize_ovens_for_window(
    lambda_per_hr: float,
    mu_per_hr: float,
    queue_m: int = 10,
    max_ovens: int = 10,
    target_max_loss_prob: Optional[float] = 0.05,
    target_max_wait_min: Optional[float] = 15.0,
    economic_config: Optional[EconomicConfig] = None,
) -> Tuple[int, Dict[int, Tuple[QueueingMetrics, Optional[EconomicMetrics]]]]:
    """Determine the optimal number of active ovens c in [1, max_ovens].

    If economic_config is provided, chooses c to maximize net profit while
    meeting quality-of-service constraints (target_max_loss_prob, target_max_wait_min).
    Otherwise chooses the minimum c that satisfies the constraints.

    Returns:
        Tuple of (optimal_c, candidate_evaluations_dict).
    """
    evaluations: Dict[int, Tuple[QueueingMetrics, Optional[EconomicMetrics]]] = {}

    best_c = 1
    best_score = -float("inf")

    for c in range(1, max_ovens + 1):
        model = MMcmModel(c=c, m=queue_m)
        q_metrics = model.calculate_metrics(lambda_rate=lambda_per_hr, mu_rate=mu_per_hr)

        econ_metrics = None
        if economic_config is not None:
            econ_metrics = evaluate_economics(
                metrics=q_metrics, config=economic_config, duration_hours=1.0, rates_in_hours=True
            )

        evaluations[c] = (q_metrics, econ_metrics)

        # Check QoS constraints
        wait_min = q_metrics.avg_wait_time_queue * 60.0
        meets_loss = (target_max_loss_prob is None) or (q_metrics.p_reject <= target_max_loss_prob)
        meets_wait = (target_max_wait_min is None) or (wait_min <= target_max_wait_min)

        if economic_config is not None:
            # Objective: Maximize net profit with penalty for violating QoS
            score = econ_metrics.net_profit
            if not meets_loss:
                score -= 1000.0 * (q_metrics.p_reject - (target_max_loss_prob or 0.0))
            if not meets_wait:
                score -= 100.0 * (wait_min - (target_max_wait_min or 0.0))

            if score > best_score:
                best_score = score
                best_c = c
        else:
            # Feasible minimum ovens that meet quality
            if meets_loss and meets_wait:
                best_c = c
                break
            else:
                best_c = c  # Fallback to highest available if none meet strictly

    return best_c, evaluations


class PizzeriaDaySchedule:
    """Manages full-day schedule and analytical performance rollup (09:00 - 22:00, 780 min)."""

    def __init__(
        self,
        windows: Optional[List[TimeWindow]] = None,
        economic_config: Optional[EconomicConfig] = None,
    ) -> None:
        self.windows: List[TimeWindow] = windows or []
        self.economic_config = economic_config or EconomicConfig()

    @classmethod
    def create_default_benchmark_schedule(
        cls, mu_per_hr: float = 6.0, economic_config: Optional[EconomicConfig] = None
    ) -> "PizzeriaDaySchedule":
        """Generate a realistic university benchmark schedule for Option 4.

        Hours: 09:00 - 22:00 (540 to 1320 min, 13 hours total).
        Five operational regimes:
            1. Morning ramp-up (09:00 - 11:30, 150 min): lambda = 8/hr, c = 2
            2. Lunch rush (11:30 - 14:00, 150 min): lambda = 36/hr, c = 7
            3. Afternoon lull (14:00 - 17:30, 210 min): lambda = 16/hr, c = 3
            4. Dinner rush (17:30 - 20:30, 180 min): lambda = 48/hr, c = 9
            5. Evening wind-down (20:30 - 22:00, 90 min): lambda = 12/hr, c = 2
        """
        windows = [
            TimeWindow("Morning Ramp-Up", 540, 690, lambda_per_hr=8.0, mu_per_hr=mu_per_hr, ovens_c=2),
            TimeWindow("Lunch Rush", 690, 840, lambda_per_hr=36.0, mu_per_hr=mu_per_hr, ovens_c=7),
            TimeWindow("Afternoon Lull", 840, 1050, lambda_per_hr=16.0, mu_per_hr=mu_per_hr, ovens_c=3),
            TimeWindow("Dinner Peak", 1050, 1230, lambda_per_hr=48.0, mu_per_hr=mu_per_hr, ovens_c=9),
            TimeWindow("Evening Closing", 1230, 1320, lambda_per_hr=12.0, mu_per_hr=mu_per_hr, ovens_c=2),
        ]
        return cls(windows=windows, economic_config=economic_config)

    def evaluate(self) -> pd.DataFrame:
        """Evaluate analytical queueing and economic metrics across all windows.

        Returns:
            A pandas DataFrame with one row per time window.
        """
        records: List[Dict] = []
        for w in self.windows:
            model = MMcmModel(c=w.ovens_c, m=w.queue_m)
            q_metrics = model.calculate_metrics(lambda_rate=w.lambda_per_hr, mu_rate=w.mu_per_hr)
            econ = evaluate_economics(
                metrics=q_metrics,
                config=self.economic_config,
                duration_hours=w.duration_hours,
                rates_in_hours=True,
            )

            record = {
                "Window": w.name,
                "Time Interval": f"{w.start_clock} - {w.end_clock}",
                "Duration (hrs)": w.duration_hours,
                "Active Ovens (c)": w.ovens_c,
                "Arrival Rate lambda (/hr)": w.lambda_per_hr,
                "Service Rate mu (/hr)": w.mu_per_hr,
                "Offered Load rho": round(q_metrics.rho, 3),
                "Oven Load psi": round(q_metrics.psi, 3),
                "Idle Prob P0": round(q_metrics.p0, 4),
                "Rejection Prob P_loss": round(q_metrics.p_reject, 4),
                "Abs Throughput A (/hr)": round(q_metrics.absolute_throughput, 3),
                "Avg Busy Ovens L_s": round(q_metrics.busy_servers, 3),
                "Oven Utilization eta": round(q_metrics.server_utilization, 3),
                "Avg Queue Length L_q": round(q_metrics.avg_queue_length, 3),
                "Avg Queue Wait W_q (min)": round(q_metrics.avg_wait_time_queue * 60.0, 2),
                "Expected Arrivals": round(econ.expected_arrivals, 1),
                "Expected Served": round(econ.expected_served_orders, 1),
                "Expected Lost": round(econ.expected_lost_orders, 1),
                "Gross Revenue (UAH)": round(econ.gross_revenue, 2),
                "Oven Cost (UAH)": round(econ.oven_operational_cost, 2),
                "Net Profit (UAH)": round(econ.net_profit, 2),
            }
            records.append(record)

        df = pd.DataFrame(records)
        return df

    def daily_summary(self) -> Dict[str, float]:
        """Aggregate whole-day totals and weighted averages across the 780-minute schedule."""
        df = self.evaluate()
        total_hours = df["Duration (hrs)"].sum()
        total_arrivals = df["Expected Arrivals"].sum()
        total_served = df["Expected Served"].sum()
        total_lost = df["Expected Lost"].sum()
        total_revenue = df["Gross Revenue (UAH)"].sum()
        total_oven_cost = df["Oven Cost (UAH)"].sum()
        total_profit = df["Net Profit (UAH)"].sum()

        # Weighted queue and wait metrics
        weighted_lq = (df["Avg Queue Length L_q"] * df["Duration (hrs)"]).sum() / total_hours
        weighted_loss_prob = total_lost / total_arrivals if total_arrivals > 0 else 0.0
        weighted_wait_min = (
            (df["Avg Queue Wait W_q (min)"] * df["Expected Served"]).sum() / total_served
            if total_served > 0
            else 0.0
        )
        avg_active_ovens = (df["Active Ovens (c)"] * df["Duration (hrs)"]).sum() / total_hours

        return {
            "Total Operating Hours": total_hours,
            "Average Active Ovens": round(avg_active_ovens, 2),
            "Total Incoming Orders": round(total_arrivals, 1),
            "Total Served Orders": round(total_served, 1),
            "Total Rejected Orders": round(total_lost, 1),
            "Overall Loss Rate": round(weighted_loss_prob, 4),
            "Time-Weighted Avg Queue Length L_q": round(weighted_lq, 3),
            "Customer-Weighted Avg Queue Wait W_q (min)": round(weighted_wait_min, 2),
            "Total Gross Revenue (UAH)": round(total_revenue, 2),
            "Total Oven Operating Cost (UAH)": round(total_oven_cost, 2),
            "Total Daily Net Profit (UAH)": round(total_profit, 2),
        }
