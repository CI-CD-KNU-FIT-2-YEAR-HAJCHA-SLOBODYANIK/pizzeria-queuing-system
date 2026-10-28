"""Analytical Queueing Theory Package for Pizzeria Optimization (M/M/c/m System).

Role 3: Theoretical Modeler & Technical Writer.
Provides exact steady-state analytical formulas, state probability distributions,
performance metrics, economic evaluation, and dynamic schedule assessment.
"""

from .solver import MMcmModel, QueueingMetrics, solve_mmcm
from .economics import EconomicConfig, EconomicMetrics, evaluate_economics
from .schedule import TimeWindow, PizzeriaDaySchedule, optimize_ovens_for_window

__all__ = [
    "MMcmModel",
    "QueueingMetrics",
    "solve_mmcm",
    "EconomicConfig",
    "EconomicMetrics",
    "evaluate_economics",
    "TimeWindow",
    "PizzeriaDaySchedule",
    "optimize_ovens_for_window",
]

__version__ = "1.0.0"
