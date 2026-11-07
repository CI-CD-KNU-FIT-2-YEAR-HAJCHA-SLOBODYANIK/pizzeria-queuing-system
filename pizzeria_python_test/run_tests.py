"""Главная программа. GUI -> JSONL/manifest -> C++ exe -> анализ -> графики.

Запуск: python run_tests.py
Без GUI: python run_tests.py --headless --exe C:\\...\\simulation_engine.exe
"""

import argparse
import csv
import json
import random
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

from analyzer import choose_parameters, read_report, summarize, write_analysis
from generator import generate_orders, save_orders

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
GENERATED = HERE / "generated"
SETTINGS_FILE = HERE / "local_settings.json"
CLOSING_TIME = 21600

DEFAULTS = {
    "exe": "", "threads": 4, "runs": 8, "generations": 2, "orders": 40,
    "max_ovens": 5, "mode": "fast", "seed": 42, "rush_fraction": 0.75,
    "recipes": str(HERE / "data" / "recipes.csv"),
    "toppings": str(HERE / "data" / "toppings.csv"),
    "radius_wait": str(HERE / "data" / "radius_wait.csv"),
}


def load_settings() -> dict:
    settings = DEFAULTS.copy()
    if SETTINGS_FILE.is_file():
        try:
            saved = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                settings.update({k: v for k, v in saved.items() if k in settings})
        except (OSError, ValueError):
            pass
    if not settings["exe"]:
        for sub in ("x64/Release", "x64/Debug", "simulation_engine/x64/Release",
                    "simulation_engine/x64/Debug"):
            candidate = REPO / sub / "simulation_engine.exe"
            if candidate.is_file():
                settings["exe"] = str(candidate.resolve())
                break
    return settings


def save_settings(settings: dict) -> None:
    SETTINGS_FILE.write_text(json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8")


def check_settings(s: dict) -> dict:
    s = s.copy()
    for k in ("threads", "runs", "generations", "orders", "max_ovens", "seed"):
        s[k] = int(s[k])
    s["rush_fraction"] = float(s["rush_fraction"])
    if not 1 <= s["threads"] <= 256 or not 1 <= s["runs"] <= 256:
        raise ValueError("Потоки и прогоны должны быть в пределах 1–256")
    if not 1 <= s["generations"] <= 100 or not 1 <= s["orders"] <= 100000:
        raise ValueError("Генерации: 1–100; заказы: 1–100000")
    if not 1 <= s["max_ovens"] <= 30:
        raise ValueError("Максимум печей: 1–30")
    if not 0 <= s["rush_fraction"] <= 1:
        raise ValueError("Доля заказов в часы пик: 0–1")
    if s["mode"] not in ("fast", "bounded"):
        raise ValueError("Режим очереди должен быть fast или bounded")
    for key in ("exe", "recipes", "toppings", "radius_wait"):
        p = Path(s[key]).expanduser().resolve()
        if not p.is_file():
            raise FileNotFoundError(f"Не найден файл {key}: {p}")
        s[key] = str(p)
    return s


def load_radius_map(path: str) -> list:
    result = []
    with Path(path).open(encoding="utf-8-sig", newline="") as file:
        for row in csv.DictReader(file):
            r = int(row["radius_cm"])
            w = int(row["wait_time"])
            if r <= 0 or w <= 0 or (result and r <= result[-1]["radius_cm"]):
                raise ValueError("radius_wait.csv: нужны положительные радиусы по возрастанию")
            result.append({"radius_cm": r, "wait_time": w})
    if not result:
        raise ValueError("radius_wait.csv пуст")
    return result


def unique_tag(generation: int) -> str:
    stamp = datetime.now().strftime("%Y%m%d_%H%M")
    index = generation
    while (GENERATED / "orders" / f"orders_{stamp}_g{index:03d}.jsonl").exists():
        index += 1
    return f"{stamp}_g{index:03d}"


def build_manifest(s: dict, tag: str, orders_path: Path, output_dir: Path,
                   candidates: list, radius_wait: list) -> dict:
    base_config = {
        "opening_time": 0, "closing_time": CLOSING_TIME,
        "ingredient_price_g": {"meat": 0.2, "dough": 0.05, "cheese": 0.1,
                               "veg": 0.04, "seafood": 0.5},
        "oven_cost_sec": 0.01, "idle_power_ratio": 0.5,
        "heat_up_sec": 0, "cool_down_sec": 0, "oven_capacity": 1,
        "radius_wait_map": radius_wait, "online_delay_sec": 0,
        "in_person_delay_sec": 0, "quant_step_min": 1, "quant_by_clock": True,
        "max_early_prep_sec": 1200, "soft_delay_limit_sec": 300,
        "static_policy": [{"time": 0, "target_ovens": 1}], "dynamic_policy": [],
    }
    return {
        "version": 1, "parallelism": s["threads"],
        "max_ovens": s["max_ovens"],
        "output_dir": str(output_dir.resolve()),
        "include_oven_events": False,
        "queue": {"mode": s["mode"], "max_search_nodes": 600,
                  "max_repack_nodes": 3000, "max_branch_per_task": 20,
                  "max_variants": 16, "max_time_sec": 86400},
        "shared": {"recipes": s["recipes"], "toppings": s["toppings"],
                   "orders": str(orders_path.resolve()), "config": base_config},
        "runs": [{"id": f"{tag}_r{i + 1:03d}", "config": c}
                 for i, c in enumerate(candidates)],
    }


def execute_cpp(s: dict, manifest_path: Path, report_dir: Path,
                log_path: Path, on_event, stop_event: threading.Event) -> list:
    """Один процесс C++ с N рабочими потоками. stdout обрабатывается построчно."""
    cmd = [s["exe"], "--manifest", str(manifest_path), "--threads", str(s["threads"]),
           "--interactive"]
    on_event("log", "C++: " + " ".join(cmd))
    rows = []
    run_progress = {}
    started_runs = set()
    with log_path.open("x", encoding="utf-8") as log, subprocess.Popen(
        cmd, cwd=str(REPO), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        bufsize=1
    ) as proc:
        polling_done = threading.Event()
        wait_sent = False

        def poll_status():
            stop_sent = False
            while not polling_done.wait(0.75):
                if proc.poll() is not None:
                    return
                try:
                    if stop_event.is_set() and not stop_sent:
                        proc.stdin.write("stop-all\n")
                        stop_sent = True
                    proc.stdin.write("status\n")
                    proc.stdin.flush()
                except (OSError, ValueError):
                    return

        monitor = threading.Thread(target=poll_status, daemon=True)
        monitor.start()
        try:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                try:
                    event = json.loads(line)
                except ValueError:
                    on_event("log", line.rstrip())
                    continue
                kind = event.get("event")
                if kind in ("progress", "status"):
                    if kind == "status":
                        for item in event.get("runs", []):
                            run_id = str(item.get("id", ""))
                            run_progress[run_id] = item
                            if run_id and item.get("state") == "running" and run_id not in started_runs:
                                started_runs.add(run_id)
                                on_event("log", "\n" + "=" * 53 +
                                         f"\nНАЧАЛО ПРОГОНА: {run_id}\n" + "=" * 53)
                    total = max(1, int(event.get("total", s["runs"])))
                    finished = int(event.get("finished", 0))
                    partial = sum(
                        max(0, min(1, int(r.get("simulation_sec", 0)) / 86400))
                        for r in run_progress.values() if r.get("state") == "running"
                    )
                    on_event("progress", (finished, total, min(100, 100 * (finished + partial) / total)))
                    # Interactive CLI ожидает команду wait даже после окончания всех работ.
                    if finished >= total and not wait_sent:
                        wait_sent = True
                        polling_done.set()
                        try:
                            proc.stdin.write("wait\n")
                            proc.stdin.flush()
                        except (OSError, ValueError):
                            pass
                elif kind == "finished":
                    run_id = str(event.get("run_id", ""))
                    if run_id not in started_runs:
                        started_runs.add(run_id)
                        on_event("log", "\n" + "=" * 53 +
                                 f"\nПРОГОН: {run_id}\n" + "=" * 53)
                    path = report_dir / (run_id + ".json")
                    if path.is_file():
                        try:
                            item = read_report(path)
                            rows.append(item)
                            on_event("result", item)
                        except (OSError, ValueError, KeyError) as exc:
                            on_event("log", f"Ошибка чтения {path.name}: {exc}")
                    else:
                        on_event("log", f"Нет отчёта {path.name}: {event.get('status')}")
                elif kind in ("fatal", "error", "output_error"):
                    on_event("log", f"Ошибка C++: {event.get('message', event)}")
                elif kind == "ready":
                    on_event("log", f"C++ начал {event.get('total')} прогонов, потоков: {event.get('parallelism')}")
            code = proc.wait()
        finally:
            polling_done.set()
            monitor.join(timeout=2)
            if proc.stdin:
                proc.stdin.close()
        on_event("log", f"C++ завершён, exit code: {code}")
    return rows


def run_experiments(s: dict, on_event, stop_event: threading.Event):
    """Работает в Python-потоке; on_event передаёт события GUI без обращения к Tk."""
    s = check_settings(s)
    save_settings(s)
    radius_wait = load_radius_map(s["radius_wait"])
    rng = random.Random(s["seed"])
    all_rows, last_orders = [], []
    previous = []
    for generation in range(1, s["generations"] + 1):
        if stop_event.is_set():
            break
        tag = unique_tag(generation)
        orders_path = GENERATED / "orders" / f"orders_{tag}.jsonl"
        manifest_path = GENERATED / "manifests" / f"manifest_{tag}.json"
        report_dir = GENERATED / "reports" / f"reports_{tag}"
        log_path = GENERATED / "logs" / f"console_{tag}.log"
        for directory in (orders_path.parent, manifest_path.parent, report_dir, log_path.parent):
            directory.mkdir(parents=True, exist_ok=True)
        seed = s["seed"] + generation - 1
        last_orders = generate_orders(Path(s["recipes"]), Path(s["toppings"]),
                                      s["orders"], seed=seed,
                                      closing_time=CLOSING_TIME,
                                      rush_fraction=s["rush_fraction"])
        save_orders(last_orders, orders_path)
        candidates = choose_parameters(previous, s["runs"], s["max_ovens"], rng)
        manifest = build_manifest(s, tag, orders_path, report_dir, candidates, radius_wait)
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        on_event("generation", (generation, s["generations"], tag))
        on_event("log", f"Заказы: {orders_path}\nManifest: {manifest_path}")
        previous = execute_cpp(s, manifest_path, report_dir, log_path, on_event, stop_event)
        all_rows.extend(previous)
        write_analysis(previous, GENERATED / "reports" / f"analysis_{tag}.json")
        stats = summarize(previous)
        on_event("log", f"Итерация {generation}: {stats['completed']}/{stats['total']} успешно, "
                        f"прибыль: {stats['total_profit']:.2f}, ошибки: {stats['issues']}")
        if stop_event.is_set():
            break
    return all_rows, last_orders


def main():
    parser = argparse.ArgumentParser(description="Тестирование C++-симулятора")
    parser.add_argument("--headless", action="store_true", help="Без графического окна")
    parser.add_argument("--exe", help="Путь к simulation_engine.exe")
    for key in ("threads", "runs", "generations", "orders", "max_ovens", "seed"):
        parser.add_argument("--" + key.replace("_", "-"), type=int)
    parser.add_argument("--mode", choices=("fast", "bounded"))
    args = parser.parse_args()
    settings = load_settings()
    for key in ("exe", "threads", "runs", "generations", "orders", "max_ovens", "seed", "mode"):
        val = getattr(args, key, None)
        if val is not None:
            settings[key] = val
    if args.headless:
        def log_event(kind, payload):
            if kind == "log": print(payload, flush=True)
            elif kind == "generation": print(f"Генерация {payload[0]}/{payload[1]}", flush=True)
            elif kind == "progress": print(f"Прогресс: {payload[0]}/{payload[1]} (~{payload[2]:.1f}%)", flush=True)
            elif kind == "result": print(f"{payload['id']}: прибыль {payload['profit']:.2f}, "
                                         f"1/2/3: {payload['issues']}", flush=True)
        rows, orders = run_experiments(settings, log_event, threading.Event())
        print("ИТОГ:", summarize(rows), flush=True)
        return
    from visualizer import launch_gui
    launch_gui(settings, run_experiments, check_settings, GENERATED)


if __name__ == "__main__":
    main()
