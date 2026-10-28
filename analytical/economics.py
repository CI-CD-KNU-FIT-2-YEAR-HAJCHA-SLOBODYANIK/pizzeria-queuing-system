"""Economic and Financial Optimization Module for Pizzeria Oven Management.

Calculates gross revenue, operational oven costs (active baking vs. idle heating),
customer loss penalties, and net operating profit for an M/M/c/m queueing system.
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import Dict
from .solver import QueueingMetrics


@dataclass(frozen=True)
class EconomicConfig:
    """Financial parameters for the pizzeria operation.

    Attributes:
        revenue_per_order: Net revenue earned per completed order (UAH).
        oven_cost_per_hour: Cost to run one active oven for one hour (UAH/hour).
        idle_power_ratio: Fraction of power/cost consumed when an active oven is idle
            (0.0 <= idle_power_ratio <= 1.0, aligns with structures.hpp idle_power_ratio).
        lost_order_penalty: Opportunity cost or brand penalty for each rejected order (UAH).
        fixed_hourly_overhead: Other fixed hourly costs (staff, rent, light) (UAH/hour).
    """

    revenue_per_order: float = 250.0  # UAH / order average
    oven_cost_per_hour: float = 60.0  # UAH / hour per running oven
    idle_power_ratio: float = 0.5  # 50% power when running but empty
    lost_order_penalty: float = 50.0  # Penalty for losing customer due to full queue
    fixed_hourly_overhead: float = 120.0  # General overhead per hour


@dataclass(frozen=True)
class EconomicMetrics:
    """Results of economic evaluation over a given time duration.

    Attributes:
        duration_hours: Length of evaluation window in hours.
        expected_arrivals: Total incoming orders during window.
        expected_served_orders: Number of successfully served orders.
        expected_lost_orders: Number of lost/rejected orders.
        gross_revenue: Total revenue earned from served orders (UAH).
        oven_operational_cost: Total energy/wear cost of running ovens (UAH).
        oven_busy_cost: Portion of cost spent while actively baking (UAH).
        oven_idle_cost: Portion of cost spent while idling (UAH).
        lost_order_penalty_cost: Cost penalty incurred from rejections (UAH).
        fixed_overhead_cost: Fixed overhead costs over the window (UAH).
        total_cost: Sum of oven costs, rejection penalties, and fixed overhead.
        net_profit: Gross revenue minus total cost (UAH).
        hourly_profit_rate: Net profit normalized to per-hour rate (UAH/hour).
    """

    duration_hours: float
    expected_arrivals: float
    expected_served_orders: float
    expected_lost_orders: float
    gross_revenue: float
    oven_operational_cost: float
    oven_busy_cost: float
    oven_idle_cost: float
    lost_order_penalty_cost: float
    fixed_overhead_cost: float
    total_cost: float
    net_profit: float
    hourly_profit_rate: float

    def summary_dict(self) -> Dict[str, float]:
        """Return a formatted dictionary of economic metrics."""
        return {
            "Window Duration (hours)": self.duration_hours,
            "Expected Arrivals": self.expected_arrivals,
            "Served Orders": self.expected_served_orders,
            "Lost Orders": self.expected_lost_orders,
            "Gross Revenue (UAH)": self.gross_revenue,
            "Oven Operating Cost (UAH)": self.oven_operational_cost,
            "Oven Busy Cost (UAH)": self.oven_busy_cost,
            "Oven Idle Cost (UAH)": self.oven_idle_cost,
            "Lost Order Penalties (UAH)": self.lost_order_penalty_cost,
            "Fixed Overhead (UAH)": self.fixed_overhead_cost,
            "Total Cost (UAH)": self.total_cost,
            "Net Profit (UAH)": self.net_profit,
            "Hourly Profit Rate (UAH/hr)": self.hourly_profit_rate,
        }


def evaluate_economics(
    metrics: QueueingMetrics,
    config: EconomicConfig,
    duration_hours: float = 1.0,
    rates_in_hours: bool = True,
) -> EconomicMetrics:
    """Evaluate economic outcomes for a queueing state over a given time duration.

    Args:
        metrics: Computed QueueingMetrics object.
        config: EconomicConfig containing prices and cost parameters.
        duration_hours: Duration of this operational window in hours.
        rates_in_hours: True if lambda_rate and mu_rate are expressed per hour;
                        False if they are expressed per minute.

    Returns:
        EconomicMetrics instance.
    """
    if duration_hours < 0.0:
        raise ValueError(f"duration_hours must be >= 0, got {duration_hours}.")

    time_multiplier = 1.0 if rates_in_hours else 60.0

    # Hourly rates
    lambda_per_hr = metrics.lambda_rate * time_multiplier
    abs_throughput_per_hr = metrics.absolute_throughput * time_multiplier

    # Order volumes over duration
    expected_arrivals = lambda_per_hr * duration_hours
    expected_served = abs_throughput_per_hr * duration_hours
    expected_lost = expected_arrivals - expected_served

    # Gross revenue
    gross_revenue = expected_served * config.revenue_per_order

    # Oven operational costs:
    # Average busy ovens: L_s
    # Average idle active ovens: c - L_s
    busy_ovens = metrics.busy_servers
    idle_active_ovens = max(0.0, float(metrics.c) - busy_ovens)

    # Cost per hour for active ovens:
    # Full cost while busy + discounted cost while idle
    busy_cost_per_hr = busy_ovens * config.oven_cost_per_hour
    idle_cost_per_hr = idle_active_ovens * config.oven_cost_per_hour * config.idle_power_ratio
    oven_cost_per_hr = busy_cost_per_hr + idle_cost_per_hr

    oven_busy_cost = busy_cost_per_hr * duration_hours
    oven_idle_cost = idle_cost_per_hr * duration_hours
    oven_operational_cost = oven_cost_per_hr * duration_hours

    # Penalties & overheads
    lost_order_penalty_cost = expected_lost * config.lost_order_penalty
    fixed_overhead_cost = config.fixed_hourly_overhead * duration_hours

    total_cost = oven_operational_cost + lost_order_penalty_cost + fixed_overhead_cost
    net_profit = gross_revenue - total_cost

    hourly_profit_rate = net_profit / duration_hours if duration_hours > 0 else 0.0

    return EconomicMetrics(
        duration_hours=duration_hours,
        expected_arrivals=expected_arrivals,
        expected_served_orders=expected_served,
        expected_lost_orders=expected_lost,
        gross_revenue=gross_revenue,
        oven_operational_cost=oven_operational_cost,
        oven_busy_cost=oven_busy_cost,
        oven_idle_cost=oven_idle_cost,
        lost_order_penalty_cost=lost_order_penalty_cost,
        fixed_overhead_cost=fixed_overhead_cost,
        total_cost=total_cost,
        net_profit=net_profit,
        hourly_profit_rate=hourly_profit_rate,
    )
