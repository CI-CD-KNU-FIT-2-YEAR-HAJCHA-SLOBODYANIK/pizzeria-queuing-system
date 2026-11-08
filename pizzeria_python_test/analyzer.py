"""
Модуль аналізу результатів імітаційного моделювання (Учасник 2).
Порівнює вихідні JSON-звіти C++ движка з теоретичною моделлю (analytical_solver.py).
"""

import json
from pathlib import Path
from typing import Dict, Any, List
from analytical_solver import solve_mmcm


def load_simulation_report(json_path: Path) -> Dict[str, Any]:
    """Зчитує вихідний JSON-звіт прогону від C++ движка."""
    if not json_path.is_file():
        raise FileNotFoundError(f"Файл звіту не знайдено: {json_path}")
    with json_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def analyze_run_vs_theory(report_path: Path) -> Dict[str, Any]:
    """Зводить дані імітації та теорії для одного прогону."""
    report = load_simulation_report(report_path)
    summary = report.get("results", {}).get("summary", {})
    config = report.get("config", {}).get("pizzeria", {})

    # Отримання практичних метрик імітації
    sim_orders_total = summary.get("orders_total", 0)
    sim_orders_rejected = summary.get("orders_rejected", 0)
    sim_p_loss = (sim_orders_rejected / sim_orders_total * 100) if sim_orders_total > 0 else 0.0
    sim_avg_wait_min = summary.get("avg_wait_sec", 0) / 60.0
    sim_profit = summary.get("profit", 0.0)

    # Отримання параметрів для аналітичного розрахунку
    max_ovens = report.get("config", {}).get("max_ovens", 4)
    opening_time = config.get("opening_time", 32400)
    closing_time = config.get("closing_time", 79200)
    day_hours = (closing_time - opening_time) / 3600.0 if closing_time > opening_time else 13.0

    lambda_rate = sim_orders_total / day_hours if day_hours > 0 else 25.0
    mu_rate = 6.0  # 10 хв на піч (6 піц/год)

    # Теоретичний розрахунок через модуль Учасника 3
    theory = solve_mmcm(c=max_ovens, m=10, lambda_rate=lambda_rate, mu_rate=mu_rate)

    # Розрахунок відносних похибок (%)
    error_p_loss = abs(sim_p_loss - (theory.p_loss * 100))
    error_wait = abs(sim_avg_wait_min - (theory.avg_wait_time_queue * 60))

    return {
        "run_id": report.get("run_id", report_path.stem),
        "active_ovens": max_ovens,
        "simulation": {
            "total_orders": sim_orders_total,
            "rejected_orders": sim_orders_rejected,
            "p_loss_percent": round(sim_p_loss, 2),
            "avg_wait_min": round(sim_avg_wait_min, 2),
            "profit_uah": round(sim_profit, 2)
        },
        "theory": {
            "p_loss_percent": round(theory.p_loss * 100, 2),
            "avg_wait_min": round(theory.avg_wait_time_queue * 60, 2),
        },
        "errors": {
            "p_loss_diff_percent": round(error_p_loss, 2),
            "wait_time_diff_min": round(error_wait, 2)
        }
    }


def print_comparison_table(results: List[Dict[str, Any]]) -> None:
    """Виводить зведену таблицю порівняння імітації та теорії."""
    print("=" * 95)
    print(f"{'Run ID':<15} | {'Печі':<5} | {'P_loss Іміт (%)':<15} | {'P_loss Теор (%)':<15} | {'Чекання Іміт (хв)':<18} | {'Прибуток (грн)':<15}")
    print("-" * 95)
    for res in results:
        sim = res["simulation"]
        th = res["theory"]
        print(f"{res['run_id']:<15} | {res['active_ovens']:<5} | {sim['p_loss_percent']:<15} | {th['p_loss_percent']:<15} | {sim['avg_wait_min']:<18} | {sim['profit_uah']:<15}")
    print("=" * 95)


if __name__ == "__main__":
    output_dir = Path("output")
    if output_dir.exists():
        reports = list(output_dir.glob("*.json"))
        parsed_results = [analyze_run_vs_theory(r) for r in reports]
        print_comparison_table(parsed_results)
    else:
        print("Папка output/ порожня або відсутня. Запустіть C++ двигун для отримання звітів.")
