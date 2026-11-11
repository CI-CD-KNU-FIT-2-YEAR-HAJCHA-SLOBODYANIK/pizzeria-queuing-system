"""
Модуль аналізу результатів імітаційного моделювання.
Агрегує серії прогонів Монте-Карло за кількістю печей та обчислює усереднені значення.
"""

import sys
import json
from pathlib import Path
from typing import Dict, Any, List
from collections import defaultdict

PROJECT_ROOT = Path(__file__).resolve().parent.parent if "__file__" in locals() else Path(".").resolve()
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

try:
    from analytical.solver import solve_mmcm
except ImportError:
    try:
        from analytical_solver import solve_mmcm
    except ImportError:
        pass


def load_simulation_report(json_path: Path) -> Dict[str, Any]:
    if not json_path.is_file():
        raise FileNotFoundError(f"Файл звіту не знайдено: {json_path}")
    with json_path.open("r", encoding="utf-8-sig") as f:
        return json.load(f)


def analyze_all_reports(output_dir: Path) -> List[Dict[str, Any]]:
    reports = list(output_dir.rglob("*.json"))
    grouped_data = defaultdict(list)

    for report_path in reports:
        if report_path.name.startswith("analysis_") or report_path.name.startswith("manifest_"):
            continue
        try:
            report = load_simulation_report(report_path)
            summary = report.get("results", {}).get("summary", {})
            config = report.get("config", {}).get("pizzeria", {})

            sim_orders_total = summary.get("orders_total", 0)
            sim_orders_rejected = summary.get("orders_rejected", 0)
            sim_p_loss = (sim_orders_rejected / sim_orders_total * 100) if sim_orders_total > 0 else 0.0
            sim_avg_wait_min = summary.get("avg_wait_sec", 0) / 60.0
            sim_profit = summary.get("profit", 0.0)

            max_ovens = report.get("config", {}).get("max_ovens", 4)
            opening_time = config.get("opening_time", 32400)
            closing_time = config.get("closing_time", 79200)
            day_hours = (closing_time - opening_time) / 3600.0 if closing_time > opening_time else 13.0

            lambda_rate = sim_orders_total / day_hours if day_hours > 0 else 25.0

            grouped_data[max_ovens].append({
                "p_loss": sim_p_loss,
                "wait_min": sim_avg_wait_min,
                "profit": sim_profit,
                "lambda_rate": lambda_rate
            })
        except Exception:
            continue

    aggregated_results = []
    for ovens in sorted(grouped_data.keys()):
        runs = grouped_data[ovens]
        count = len(runs)

        avg_p_loss = sum(r["p_loss"] for r in runs) / count
        avg_wait = sum(r["wait_min"] for r in runs) / count
        avg_profit = sum(r["profit"] for r in runs) / count
        avg_lambda = sum(r["lambda_rate"] for r in runs) / count

        try:
            theory = solve_mmcm(c=ovens, m=10, lambda_rate=avg_lambda, mu_rate=6.0)
            p_loss_theor = theory.p_loss * 100
            avg_wait_theor = theory.avg_wait_time_queue * 60
        except Exception:
            p_loss_theor = 0.0
            avg_wait_theor = 0.0

        aggregated_results.append({
            "active_ovens": ovens,
            "runs_count": count,
            "avg_p_loss": round(avg_p_loss, 2),
            "theory_p_loss": round(p_loss_theor, 2),
            "avg_wait_min": round(avg_wait, 2),
            "theory_wait_min": round(avg_wait_theor, 2),
            "avg_profit": round(avg_profit, 2)
        })

    return aggregated_results


def print_comparison_table(results: List[Dict[str, Any]]) -> None:
    print("=" * 95)
    print(f"{'Печі':<5} | {'Прогонів':<8} | {'P_loss Іміт (%)':<15} | {'P_loss Теор (%)':<15} | {'Чекання (хв)':<13} | {'Сер. Прибуток (грн)':<20}")
    print("-" * 95)
    for res in results:
        print(f"{res['active_ovens']:<5} | {res['runs_count']:<8} | {res['avg_p_loss']:<15} | {res['theory_p_loss']:<15} | {res['avg_wait_min']:<13} | {res['avg_profit']:<20}")
    print("=" * 95)


if __name__ == "__main__":
    output_dir = Path("output")
    if output_dir.exists():
        res = analyze_all_reports(output_dir)
        print_comparison_table(res)
    else:
        print(f"Папка {output_dir} порожня або відсутня. Запустіть C++ двигун для отримання звітів.")
