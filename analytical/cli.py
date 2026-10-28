"""Command-line interface (CLI) for running analytical M/M/c/m evaluations.

Usage examples:
    python -m analytical.cli --benchmark
    python -m analytical.cli --single --c 5 --m 10 --lambda-rate 28.0 --mu-rate 6.0
    python -m analytical.cli --optimize --lambda-rate 36.0 --mu-rate 6.0
    python -m analytical.cli --export-csv "analytical_benchmark.csv"
"""

from __future__ import annotations
import argparse
import sys
import pandas as pd

from .solver import MMcmModel, solve_mmcm
from .economics import EconomicConfig, evaluate_economics
from .schedule import PizzeriaDaySchedule, optimize_ovens_for_window


def run_single(c: int, m: int, lam: float, mu: float) -> None:
    print("=" * 72)
    print(f"ANALYTICAL M/M/c/m EVALUATION: c={c}, m={m}, lambda={lam}/hr, mu={mu}/hr")
    print("=" * 72)
    metrics = solve_mmcm(c=c, m=m, lambda_rate=lam, mu_rate=mu)
    for k, v in metrics.summary_dict().items():
        if isinstance(v, float):
            print(f"  {k:<35}: {v:12.4f}")
        else:
            print(f"  {k:<35}: {v:>12}")

    print("\nState Probabilities P_k (first 10 states + rejection state):")
    for k in range(min(11, metrics.capacity_K + 1)):
        tag = " (Idle)" if k == 0 else (" (All Ovens Busy)" if k == c else "")
        print(f"  P({k:<2}){tag:<18}: {metrics.state_probabilities[k]:.6f}")
    if metrics.capacity_K > 10:
        print(f"  P({metrics.capacity_K:<2}) (Rejection state) : {metrics.p_reject:.6f}")

    econ_cfg = EconomicConfig()
    econ = evaluate_economics(metrics, econ_cfg, duration_hours=1.0, rates_in_hours=True)
    print("\nEconomic Metrics (per 1 hour):")
    for k, v in econ.summary_dict().items():
        print(f"  {k:<35}: {v:12.2f}")
    print("=" * 72)


def run_benchmark(export_path: str = "") -> None:
    print("=" * 80)
    print("PIZZERIA 780-MINUTE SCHEDULE ANALYTICAL EVALUATION (09:00 - 22:00)")
    print("=" * 80)
    schedule = PizzeriaDaySchedule.create_default_benchmark_schedule(mu_per_hr=6.0)
    df = schedule.evaluate()

    # Configure pandas display
    pd.set_option("display.max_columns", None)
    pd.set_option("display.width", 120)
    print(df.to_string(index=False))

    print("\n" + "=" * 80)
    print("DAILY AGGREGATE SUMMARY (13 HOURS / 780 MINUTES):")
    print("=" * 80)
    summary = schedule.daily_summary()
    for k, v in summary.items():
        print(f"  {k:<45}: {v:12.2f}")
    print("=" * 80)

    if export_path:
        df.to_csv(export_path, index=False)
        print(f"\n[+] Successfully exported benchmark table to: {export_path}")


def run_optimization(lam: float, mu: float, m: int = 10) -> None:
    print("=" * 76)
    print(f"ACTIVE OVEN CAPACITY OPTIMIZATION: lambda={lam}/hr, mu={mu}/hr (queue m={m})")
    print("=" * 76)
    econ_cfg = EconomicConfig()
    best_c, evals = optimize_ovens_for_window(
        lambda_per_hr=lam,
        mu_per_hr=mu,
        queue_m=m,
        max_ovens=10,
        target_max_loss_prob=0.05,
        target_max_wait_min=15.0,
        economic_config=econ_cfg,
    )

    rows = []
    for c, (q_m, e_m) in evals.items():
        rows.append(
            {
                "Ovens c": c,
                "Traffic psi": round(q_m.psi, 3),
                "Loss P_rej": round(q_m.p_reject, 4),
                "Abs Thru A": round(q_m.absolute_throughput, 2),
                "Busy L_s": round(q_m.busy_servers, 2),
                "Queue L_q": round(q_m.avg_queue_length, 2),
                "Wait W_q(min)": round(q_m.avg_wait_time_queue * 60.0, 2),
                "Net Profit/hr": round(e_m.net_profit, 2) if e_m else 0.0,
                "Optimal": ">>> OPTIMAL <<<" if c == best_c else "",
            }
        )
    df = pd.DataFrame(rows)
    print(df.to_string(index=False))
    print("=" * 76)
    print(f"Recommended Active Ovens: c* = {best_c}")
    print("=" * 76)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="M/M/c/m Analytical Queueing Theory Solver for Pizzeria Optimization"
    )
    parser.add_argument("--benchmark", action="store_true", help="Run full-day 780-min schedule benchmark")
    parser.add_argument("--single", action="store_true", help="Run single steady-state calculation")
    parser.add_argument("--optimize", action="store_true", help="Find optimal active ovens c for given load")
    parser.add_argument("--c", type=int, default=4, help="Active ovens (default: 4)")
    parser.add_argument("--m", type=int, default=10, help="Queue capacity (default: 10)")
    parser.add_argument("--lambda-rate", type=float, default=24.0, help="Arrival rate orders/hr (default: 24.0)")
    parser.add_argument("--mu-rate", type=float, default=6.0, help="Service rate per oven orders/hr (default: 6.0)")
    parser.add_argument("--export-csv", type=str, default="", help="Path to export benchmark results as CSV")

    args = parser.parse_args()

    if args.single:
        run_single(c=args.c, m=args.m, lam=args.lambda_rate, mu=args.mu_rate)
    elif args.optimize:
        run_optimization(lam=args.lambda_rate, mu=args.mu_rate, m=args.m)
    elif args.benchmark or args.export_csv:
        run_benchmark(export_path=args.export_csv)
    else:
        # Default behavior: run benchmark
        run_benchmark()


if __name__ == "__main__":
    main()
