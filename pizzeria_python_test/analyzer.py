"""Чтение C++-отчётов и простое наследование удачных конфигураций."""

import json
import random
from collections import Counter
from pathlib import Path


DEFAULT_SHIFT = 21600


def read_report(path: Path) -> dict:
    """Возвращает краткий результат; ошибки чтения не считаются прибылью."""
    with Path(path).open(encoding="utf-8-sig") as file:
        data = json.load(file)
    result = data.get("results") or {}
    summary = result.get("summary") or {}
    levels = Counter()
    for item in result.get("notifications", []):
        try:
            level = int(item.get("severity", 0))
        except (TypeError, ValueError):
            continue
        if level in (1, 2, 3):
            levels[level] += 1
    cfg = (data.get("config") or {}).get("pizzeria") or {}
    shift = max(1, int(cfg.get("closing_time", DEFAULT_SHIFT)) - int(cfg.get("opening_time", 0)))
    active = float(summary.get("total_oven_active_sec", 0)) / shift
    return {
        "id": str(data.get("run_id", Path(path).stem)),
        "status": data.get("status", "unknown"),
        "profit": float(summary.get("profit", 0)),
        "revenue": float(summary.get("revenue", 0)),
        "fulfilled": int(summary.get("orders_fulfilled", 0)),
        "rejected": int(summary.get("orders_rejected", 0)),
        "wait": int(summary.get("avg_wait_sec", 0)),
        "avg_active_ovens": active,
        "issues": (levels[1], levels[2], levels[3]),
        "failure": data.get("failure"),
        "config": cfg,
        "max_ovens": int((data.get("config") or {}).get("max_ovens", 4)),
    }


def choose_parameters(previous: list, count: int, max_ovens: int,
                      rng: random.Random) -> list:
    """40% лучших полных прогонов становятся родителями; часть кандидатов случайная."""
    successful = sorted(
        (x for x in previous if x.get("status") == "completed" and x["issues"][2] == 0),
        key=lambda x: x["profit"], reverse=True
    )
    parents = successful[:max(1, (len(successful) + 1) // 2)]
    variants = []
    for i in range(count):
        if parents and i < max(1, round(count * 0.7)):
            parent = rng.choice(parents)
            c = parent["config"]
            stat = c.get("static_policy") or []
            off = int(stat[0]["target_ovens"]) if stat else 1
            peak = max(int(e["target_ovens"]) for e in stat) if stat else 1
            start = int(stat[1]["time"]) if len(stat) >= 2 else 5400
            end = int(stat[2]["time"]) if len(stat) >= 3 else 17400
            off = max(1, min(max_ovens, off + rng.choice((-1, 0, 0, 1))))
            peak = max(off, min(max_ovens, peak + rng.choice((-1, 0, 1))))
            start = max(1200, min(10500, start + rng.randint(-1200, 1200)))
            end = max(start + 1800, min(19800, end + rng.randint(-1200, 1200)))
        else:
            off = rng.randint(1, min(2, max_ovens))
            peak = rng.randint(off, max_ovens)
            start = rng.randint(3600, 9000)
            end = rng.randint(13800, 19200)
        variants.append({
            "static_policy": [
                {"time": 0, "target_ovens": off},
                {"time": start, "target_ovens": peak},
                {"time": end, "target_ovens": off},
            ],
            "heat_up_sec": rng.choice((0, 30, 60, 120)),
            "max_early_prep_sec": rng.choice((600, 900, 1200, 1800)),
        })
    return variants


def summarize(rows: list) -> dict:
    complete = [r for r in rows if r.get("status") == "completed"]
    levels = [sum(r["issues"][i] for r in rows) for i in range(3)]
    best = max(complete, key=lambda r: r["profit"], default=None)
    return {"completed": len(complete), "total": len(rows), "issues": levels,
            "best": best, "total_profit": sum(r["profit"] for r in complete)}


def write_analysis(rows: list, path: Path) -> None:
    """Сохраняет компактный итог рядом с отчётами."""
    compact = [{k: v for k, v in r.items() if k != "config"} for r in rows]
    with Path(path).open("w", encoding="utf-8") as file:
        json.dump(compact, file, ensure_ascii=False, indent=2)
