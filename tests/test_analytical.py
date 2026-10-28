"""Comprehensive Unit Tests for M/M/c/m Analytical Queueing Module and Economics.

Validates:
    - Normalization condition: sum(P_k) == 1.0
    - Flow conservation: L_s == A / mu
    - Little's Law: L_q == A * W_q, L == A * W
    - Boundary behavior at psi == 1.0, psi < 1.0, psi > 1.0
    - Single server M/M/1/m textbook comparison
    - Economic evaluations and net profit balance
    - Full-day 780-minute schedule aggregation
"""

import math
import pytest
from analytical.solver import MMcmModel, solve_mmcm
from analytical.economics import EconomicConfig, evaluate_economics
from analytical.schedule import PizzeriaDaySchedule, optimize_ovens_for_window


class TestMMcmAnalyticalSolver:

    @pytest.mark.parametrize("c,m,lam,mu", [
        (1, 5, 4.0, 5.0),
        (2, 10, 10.0, 6.0),
        (4, 10, 24.0, 6.0),
        (5, 10, 30.0, 6.0),  # psi == 1.0 critical point!
        (7, 10, 36.0, 6.0),
        (10, 10, 70.0, 6.0), # Heavy load
    ])
    def test_probability_distribution_normalization(self, c: int, m: int, lam: float, mu: float):
        """Verify that state probabilities sum to 1.0 with high precision."""
        model = MMcmModel(c=c, m=m)
        p0, probs = model.compute_state_probabilities(lambda_rate=lam, mu_rate=mu)
        total_prob = sum(probs.values())

        assert math.isclose(total_prob, 1.0, rel_tol=1e-12, abs_tol=1e-12)
        assert 0.0 < p0 <= 1.0
        for k, p in probs.items():
            assert 0.0 <= p <= 1.0, f"Probability P({k}) = {p} is out of bounds!"

    @pytest.mark.parametrize("c,m,lam,mu", [
        (3, 10, 15.0, 6.0),
        (4, 10, 24.0, 6.0),  # psi == 1.0
        (6, 10, 30.0, 5.0),  # psi == 1.0
        (8, 10, 56.0, 6.0),
    ])
    def test_littles_law_and_conservation_of_flow(self, c: int, m: int, lam: float, mu: float):
        """Validate Little's Law (L_q = A * W_q, L = A * W) and flow conservation (L_s = A / mu)."""
        metrics = solve_mmcm(c=c, m=m, lambda_rate=lam, mu_rate=mu)

        # 1. Flow conservation
        expected_ls = metrics.absolute_throughput / mu
        assert math.isclose(metrics.busy_servers, expected_ls, rel_tol=1e-9)

        # 2. System population decomposition: L = L_s + L_q
        assert math.isclose(metrics.avg_system_entities, metrics.busy_servers + metrics.avg_queue_length, rel_tol=1e-9)

        # 3. Little's law for queue: L_q = A * W_q
        assert math.isclose(metrics.avg_queue_length, metrics.absolute_throughput * metrics.avg_wait_time_queue, rel_tol=1e-9)

        # 4. Little's law for entire system: L = A * W
        assert math.isclose(metrics.avg_system_entities, metrics.absolute_throughput * metrics.avg_time_in_system, rel_tol=1e-9)

    def test_psi_equals_one_singularity(self):
        """Special test for psi = 1.0 where (1 - psi) denominator vanishes."""
        c = 4
        m = 10
        mu = 5.0
        lam = c * mu  # rho = 4, psi = 1.0

        metrics = solve_mmcm(c=c, m=m, lambda_rate=lam, mu_rate=mu)
        assert math.isclose(metrics.psi, 1.0, rel_tol=1e-12)
        assert math.isfinite(metrics.avg_queue_length)
        assert metrics.avg_queue_length > 0.0

        # Direct manual sum verification
        direct_lq = sum(j * metrics.state_probabilities[c + j] for j in range(1, m + 1))
        assert math.isclose(metrics.avg_queue_length, direct_lq, rel_tol=1e-9)

    def test_zero_arrival_boundary(self):
        """Verify boundary condition when lambda == 0."""
        metrics = solve_mmcm(c=3, m=10, lambda_rate=0.0, mu_rate=6.0)
        assert metrics.p0 == 1.0
        assert metrics.p_reject == 0.0
        assert metrics.absolute_throughput == 0.0
        assert metrics.busy_servers == 0.0
        assert metrics.avg_queue_length == 0.0
        assert metrics.avg_wait_time_queue == 0.0

    def test_textbook_mm1m(self):
        """Compare c=1 case against analytical M/M/1/m standard formula:
        P_k = (1 - rho) / (1 - rho^(m+1)) * rho^k
        """
        c = 1
        m = 5
        lam = 3.0
        mu = 4.0
        rho = lam / mu  # 0.75

        metrics = solve_mmcm(c=c, m=m, lambda_rate=lam, mu_rate=mu)

        # Textbook M/M/1/K where K = c + m is total system capacity:
        # P_0 = (1 - rho) / (1 - rho^(K + 1))
        K = c + m
        expected_p0 = (1.0 - rho) / (1.0 - (rho ** (K + 1)))
        assert math.isclose(metrics.p0, expected_p0, rel_tol=1e-9)

        expected_preject = expected_p0 * (rho ** K)
        assert math.isclose(metrics.p_reject, expected_preject, rel_tol=1e-9)


class TestEconomicEvaluation:

    def test_economic_profit_equation(self):
        """Check economic calculation consistency: Net Profit = Revenue - Costs."""
        metrics = solve_mmcm(c=5, m=10, lambda_rate=25.0, mu_rate=6.0)
        config = EconomicConfig(
            revenue_per_order=200.0,
            oven_cost_per_hour=50.0,
            idle_power_ratio=0.5,
            lost_order_penalty=40.0,
            fixed_hourly_overhead=100.0,
        )
        duration_hrs = 2.5
        econ = evaluate_economics(metrics, config, duration_hours=duration_hrs, rates_in_hours=True)

        assert math.isclose(econ.expected_arrivals, metrics.lambda_rate * duration_hrs, rel_tol=1e-9)
        assert math.isclose(econ.expected_served_orders, metrics.absolute_throughput * duration_hrs, rel_tol=1e-9)
        assert math.isclose(econ.expected_arrivals, econ.expected_served_orders + econ.expected_lost_orders, rel_tol=1e-9)

        expected_rev = econ.expected_served_orders * 200.0
        assert math.isclose(econ.gross_revenue, expected_rev, rel_tol=1e-9)

        expected_total_cost = econ.oven_operational_cost + econ.lost_order_penalty_cost + econ.fixed_overhead_cost
        assert math.isclose(econ.total_cost, expected_total_cost, rel_tol=1e-9)
        assert math.isclose(econ.net_profit, econ.gross_revenue - econ.total_cost, rel_tol=1e-9)


class TestPizzeriaSchedule:

    def test_benchmark_schedule_aggregation(self):
        """Verify that the 780-minute (13 hour) default schedule totals align correctly."""
        schedule = PizzeriaDaySchedule.create_default_benchmark_schedule(mu_per_hr=6.0)
        summary = schedule.daily_summary()

        assert math.isclose(summary["Total Operating Hours"], 13.0, rel_tol=1e-9)
        assert summary["Total Incoming Orders"] > summary["Total Served Orders"]
        assert summary["Total Daily Net Profit (UAH)"] > 0.0

    def test_oven_optimization_search(self):
        """Verify that optimizer finds an optimal c within [1, 10]."""
        best_c, evals = optimize_ovens_for_window(
            lambda_per_hr=30.0,
            mu_per_hr=6.0,
            queue_m=10,
            max_ovens=10,
            target_max_loss_prob=0.05,
        )
        assert 1 <= best_c <= 10
        opt_metrics, _ = evals[best_c]
        assert opt_metrics.p_reject <= 0.05
