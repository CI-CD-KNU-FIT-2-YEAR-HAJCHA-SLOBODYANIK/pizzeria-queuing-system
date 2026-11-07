"""Генерация заказов JSONL из CSV пицц и топпингов (Python 3.9+, без зависимостей)."""

import argparse
import csv
import json
import random
from pathlib import Path
from typing import Optional

INGREDIENTS = ("meat", "dough", "cheese", "veg", "seafood")
RECIPE_FIELDS = {"id", "name", "radius_cm", "price_base", "chef_time_sec", "oven_time_sec"}
TOPPING_FIELDS = {"id", "name", "price", "radius_cm", "chef_add_sec", "chef_mult",
                  "oven_add_sec", "oven_mult"}


def load_csv(path: Path, required: set, allow_empty: bool = False) -> list[dict]:
    if not path.is_file():
        raise FileNotFoundError(f"Таблица не найдена: {path}")
    with path.open("r", encoding="utf-8-sig", newline="") as file:
        reader = csv.DictReader(file)
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path}: отсутствуют столбцы {sorted(required - set(reader.fieldnames or []))}")
        rows = list(reader)
    if not rows and not allow_empty:
        raise ValueError(f"Таблица пуста: {path}")
    for lineno, row in enumerate(rows, 2):
        if None in row or not row.get("id") or not row.get("name", "").strip():
            raise ValueError(f"{path}, строка {lineno}: неверные id/name или число столбцов")
        try:
            row["id"] = int(row["id"])
            if row["id"] < 0:
                raise ValueError("отрицательный ID")
            row["radius_cm"] = int(row["radius_cm"]) if row.get("radius_cm", "").strip() else None
            if row["radius_cm"] is not None and row["radius_cm"] <= 0:
                raise ValueError("радиус должен быть положительным")
            if "price_base" in row:
                row["chef_time_sec"] = int(row["chef_time_sec"])
                row["oven_time_sec"] = int(row["oven_time_sec"])
                row["price_base"] = float(row["price_base"])
                if row["radius_cm"] is None or row["chef_time_sec"] < 0 or row["oven_time_sec"] <= 0:
                    raise ValueError("некорректные времена или радиус")
                for field in INGREDIENTS:
                    row[f"{field}_g"] = float(row.get(f"{field}_g") or 0)
            else:
                row["price"] = float(row["price"])
                for key in ("chef_add_sec", "oven_add_sec"):
                    row[key] = int(row.get(key) or 0)
                for key in ("chef_mult", "oven_mult"):
                    row[key] = float(row.get(key) or 1)
                for field in INGREDIENTS:
                    row[f"{field}_mult"] = float(row.get(f"{field}_mult") or 1)
                    row[f"{field}_add_g"] = float(row.get(f"{field}_add_g") or 0)
        except (ValueError, TypeError) as error:
            raise ValueError(f"{path}, строка {lineno}: некорректное число ({error})") from error
    ids = [row["id"] for row in rows]
    if len(ids) != len(set(ids)):
        raise ValueError(f"{path}: ID должны быть уникальны (включая пиццы разных размеров)")
    return rows


def choose_toppings(pizza: dict, toppings: list[dict], rng: random.Random) -> list[int]:
    """Не выбирает несовместимые радиусы, пустые эффекты и повторные множители."""
    desired = rng.choices([0, 1, 2, 3], weights=[35, 40, 20, 5])[0]
    if desired == 0:
        return []
    candidates = toppings[:]
    rng.shuffle(candidates)
    selected = []
    multiplied = set()  # Одно удвоение ингредиента/времени на пиццу.
    for topping in candidates:
        if len(selected) >= desired:
            break
        if topping["radius_cm"] not in (None, pizza["radius_cm"]):
            continue
        nonneutral = []
        valid = True
        for name in INGREDIENTS:
            key = f"{name}_mult"
            if topping[key] != 1:
                # Множитель должен действовать на изначально присутствующий ингредиент.
                if pizza[f"{name}_g"] <= 0 or key in multiplied:
                    valid = False
                    break
                nonneutral.append(key)
        if not valid:
            continue
        for key, base in (("chef_mult", pizza["chef_time_sec"]),
                          ("oven_mult", pizza["oven_time_sec"])):
            if topping[key] != 1:
                if base <= 0 or key in multiplied:
                    valid = False
                    break
                nonneutral.append(key)
        if not valid:
            continue
        # Цена сама по себе не считается эффектом: меняются ресурсы либо длительность.
        has_effect = bool(nonneutral) or any(
            topping[key] != 0 for key in ("chef_add_sec", "oven_add_sec")
        ) or any(topping[f"{name}_add_g"] != 0 for name in INGREDIENTS)
        if not has_effect:
            continue
        selected.append(topping["id"])
        multiplied.update(nonneutral)
    return selected


def generate_orders(recipes: Path, toppings: Path, count: int,
                    seed: Optional[int] = None, closing_time: int = 21600,
                    rush_fraction: float = 0.75) -> list[dict]:
    """Генерирует JSONL-заказы. Основная нагрузка приходится на два часа пик."""
    if count < 1 or closing_time < 2400:
        raise ValueError("Нужен хотя бы один заказ и смена длиной >= 2400 секунд")
    if not 0 <= rush_fraction <= 1:
        raise ValueError("rush_fraction должен быть в диапазоне 0..1")
    pizzas = load_csv(recipes, RECIPE_FIELDS | {f"{x}_g" for x in INGREDIENTS})
    addons = load_csv(toppings, TOPPING_FIELDS |
                      {f"{x}_mult" for x in INGREDIENTS} |
                      {f"{x}_add_g" for x in INGREDIENTS}, allow_empty=True)
    rng = random.Random(seed)
    upper = closing_time - 1800

    def order_time() -> int:
        # 75% заказов попадают в две зоны повышенной нагрузки (примерно 35% и 70% смены).
        if rng.random() < rush_fraction:
            center = closing_time * rng.choice((0.35, 0.70))
            return round(max(30, min(upper, rng.gauss(center, closing_time * 0.065))))
        return rng.randint(30, upper)

    timestamps = sorted(order_time() for _ in range(count))
    orders = []
    for order_id, timestamp in enumerate(timestamps, 1):
        tasks = []
        for _ in range(rng.randint(1, 3)):
            pizza = rng.choice(pizzas)
            tasks.append({"pizza_id": pizza["id"],
                          "topping_ids": choose_toppings(pizza, addons, rng)})
        orders.append({
            "id": order_id,
            "timestamp_sec": timestamp,
            "source": rng.choice(["online", "in_person"]),
            "customer_type": rng.choice(["new", "regular"]),
            "target_time": 0,
            "min_wait_sec": 0,
            "max_wait_sec": rng.choice([1800, 2400, 3600]),
            "delivery_time_sec": rng.choice([0, 60, 120]),
            "tasks": tasks,
        })
    return orders


def save_orders(orders: list[dict], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as file:
        for order in orders:
            file.write(json.dumps(order, ensure_ascii=False, allow_nan=False) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Генератор заказов Pizzeria")
    parser.add_argument("--recipes", type=Path, default=Path(__file__).parent / "data" / "recipes.csv")
    parser.add_argument("--toppings", type=Path, default=Path(__file__).parent / "data" / "toppings.csv")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--orders", type=int, default=20)
    parser.add_argument("--seed", type=int)
    args = parser.parse_args()
    orders = generate_orders(args.recipes, args.toppings, args.orders, args.seed)
    save_orders(orders, args.output)
    print(f"Сгенерировано {len(orders)} заказов: {args.output.resolve()}")


if __name__ == "__main__":
    main()
