"""
Генератор замовлень Pizzeria (JSONL) для C++ Simulation Engine.
"""

import argparse
import csv
import json
import random
from pathlib import Path
from typing import Optional, List, Dict, Any

INGREDIENTS = ("meat", "dough", "cheese", "veg", "seafood")
RECIPE_FIELDS = {"id", "name", "radius_cm", "price_base", "chef_time_sec", "oven_time_sec"}
TOPPING_FIELDS = {"id", "name", "price", "radius_cm", "chef_add_sec", "chef_mult", "oven_add_sec", "oven_mult"}


def load_csv(path: Path, required: set, allow_empty: bool = False) -> List[Dict[str, Any]]:
    """Завантаження та сувора валідація входів з CSV-файлів."""
    if not path.is_file():
        raise FileNotFoundError(f"Таблицю не знайдено: {path}")
    with path.open("r", encoding="utf-8-sig", newline="") as file:
        reader = csv.DictReader(file)
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path}: відсутні столбці {sorted(required - set(reader.fieldnames or []))}")
        rows = list(reader)
    if not rows and not allow_empty:
        raise ValueError(f"Таблиця порожня: {path}")

    for lineno, row in enumerate(rows, 2):
        if None in row or not row.get("id") or not row.get("name", "").strip():
            raise ValueError(f"{path}, строка {lineno}: некорректні id/name або число столбців")
        try:
            row["id"] = int(row["id"])
            if row["id"] < 0:
                raise ValueError("отрицательный ID")
            row["radius_cm"] = int(row["radius_cm"]) if row.get("radius_cm", "").strip() else None
            if row["radius_cm"] is not None and row["radius_cm"] <= 0:
                raise ValueError("радіус має бути додатнім")

            if "price_base" in row:
                row["chef_time_sec"] = int(row["chef_time_sec"])
                row["oven_time_sec"] = int(row["oven_time_sec"])
                row["price_base"] = float(row["price_base"])
                if row["radius_cm"] is None or row["chef_time_sec"] < 0 or row["oven_time_sec"] <= 0:
                    raise ValueError("некоректний час або радіус")
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
            raise ValueError(f"{path}, строка {lineno}: некоректне число ({error})") from error

    ids = [row["id"] for row in rows]
    if len(ids) != len(set(ids)):
        raise ValueError(f"{path}: ID повинні бути унікальні")
    return rows


def choose_toppings(pizza: Dict[str, Any], toppings: List[Dict[str, Any]], rng: random.Random) -> List[int]:
    """Вибір сумісних добавок без конфліктів за інгредієнтами та радіусом."""
    desired = rng.choices([0, 1, 2, 3], weights=[30, 45, 20, 5])[0]
    if desired == 0 or not toppings:
        return []

    candidates = toppings[:]
    rng.shuffle(candidates)
    selected = []
    multiplied = set()

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
                # Множник повинен діяти лише на наявний інгредієнт
                if pizza[f"{name}_g"] <= 0 or key in multiplied:
                    valid = False
                    break
                nonneutral.append(key)
        if not valid:
            continue

        for key, base in (("chef_mult", pizza["chef_time_sec"]), ("oven_mult", pizza["oven_time_sec"])):
            if topping[key] != 1:
                if base <= 0 or key in multiplied:
                    valid = False
                    break
                nonneutral.append(key)
        if not valid:
            continue

        has_effect = bool(nonneutral) or any(
            topping[key] != 0 for key in ("chef_add_sec", "oven_add_sec")
        ) or any(topping[f"{name}_add_g"] != 0 for name in INGREDIENTS)

        if not has_effect:
            continue

        selected.append(topping["id"])
        multiplied.update(nonneutral)

    return selected


def calculate_wait_tolerance(
    source: str,
    customer_type: str,
    pizza_count: int,
    base_bake_time_sec: int,
    delivery_time_sec: int,
    rng: random.Random
) -> tuple[int, int]:
    """
    Обчислення min_wait_sec та max_wait_sec згідно з бізнес-правилами Учасника 1:
    - Новий очний клієнт: мін терпіння не менше 20 хв (1200 сек).
    - Постійні та онлайн клієнти мають вищі ліміти.
    - Кілька піц розширюють ліміти.
    """
    # Базовий мінімальний поріг: 20 хв (1200 с) або час випікання найдовшої піци + 5 хв
    min_base = max(1200, base_bake_time_sec + 300)

    # Множник терпіння залежно від джерела та типу клієнта
    patience_mult = 1.0
    if source == "online":
        patience_mult += 0.4  # Онлайн-клієнти більш терплячі
    if customer_type == "regular":
        patience_mult += 0.3  # Постійні клієнти лояльніші

    # Додатковий час за кілька піц у замовленні
    pizza_bonus_sec = (pizza_count - 1) * 300

    # Обчислення діапазону терпіння (у секундах)
    max_wait = int((min_base + pizza_bonus_sec + delivery_time_sec) * patience_mult)
    max_wait += rng.randint(180, 600)  # Варіативність / рандомізація

    # Для очних нових клієнтів гарантуємо не менше 20 хвилин терпіння (1200 сек)
    if source == "in_person" and customer_type == "new":
        max_wait = max(1200, max_wait)

    min_wait = 0  # Стандартне очікування від моменту замовлення
    return min_wait, max_wait


def generate_orders(
    recipes: Path,
    toppings: Path,
    count: int,
    seed: Optional[int] = None,
    opening_time: int = 32400,  # 09:00 (32400 сек)
    closing_time: int = 79200,  # 22:00 (79200 сек)
) -> List[Dict[str, Any]]:
    """Генерує список замовлень з незалежними графіками попиту для онлайн та офлайн."""
    pizzas = load_csv(recipes, RECIPE_FIELDS | {f"{x}_g" for x in INGREDIENTS})
    addons = load_csv(toppings, TOPPING_FIELDS | {f"{x}_mult" for x in INGREDIENTS} | {f"{x}_add_g" for x in INGREDIENTS}, allow_empty=True)

    rng = random.Random(seed)
    day_duration = closing_time - opening_time

    timestamps = []
    sources = []

    # Розподіл попиту: 60% онлайн, 40% офлайн
    for _ in range(count):
        is_online = rng.random() < 0.6
        sources.append("online" if is_online else "in_person")

        # Незалежні піки попиту
        if is_online:
            # Онлайн піки: Обідній (35% дня) та Вечірній розтягнутий (75% дня)
            center_ratio = rng.choice([0.35, 0.75])
            sigma = day_duration * 0.08
        else:
            # Офлайн піки: Обід (30% дня) та Вечірній швидший спад (65% дня)
            center_ratio = rng.choice([0.30, 0.65])
            sigma = day_duration * 0.06

        t_sec = int(opening_time + rng.gauss(center_ratio * day_duration, sigma))
        # Обмеження часом роботи смени
        t_sec = max(opening_time + 60, min(closing_time - 900, t_sec))
        timestamps.append(t_sec)

    # Сортування замовлень строго за зростанням часу timestamp_sec
    order_data = sorted(zip(timestamps, sources), key=lambda x: x[0])

    orders = []
    for order_id, (timestamp_sec, source) in enumerate(order_data, 1):
        customer_type = "regular" if rng.random() < 0.45 else "new"

        # Замовлення "на майбутнє" (target_time > 0) у ~15% випадків для онлайн
        target_time = 0
        if source == "online" and rng.random() < 0.15:
            target_time = timestamp_sec + rng.randint(1800, 5400)
            target_time = min(closing_time, target_time)

        # Формування списку піц у замовленні (від 1 до 3 піц)
        pizza_count = rng.choices([1, 2, 3], weights=[60, 30, 10])[0]
        tasks = []
        max_pizza_bake_sec = 0

        for _ in range(pizza_count):
            pizza = rng.choice(pizzas)
            topping_ids = choose_toppings(pizza, addons, rng)
            tasks.append({"pizza_id": pizza["id"], "topping_ids": topping_ids})
            max_pizza_bake_sec = max(max_pizza_bake_sec, pizza["chef_time_sec"] + pizza["oven_time_sec"])

        # Час доставки
        delivery_time_sec = 0
        if source == "online":
            delivery_time_sec = rng.choice([600, 900, 1200, 1500])

        # Розрахунок терпіння клієнта
        min_wait_sec, max_wait_sec = calculate_wait_tolerance(
            source, customer_type, pizza_count, max_pizza_bake_sec, delivery_time_sec, rng
        )

        orders.append({
            "id": order_id,
            "timestamp_sec": timestamp_sec,
            "source": source,
            "customer_type": customer_type,
            "target_time": target_time,
            "min_wait_sec": min_wait_sec,
            "max_wait_sec": max_wait_sec,
            "delivery_time_sec": delivery_time_sec,
            "tasks": tasks
        })

    return orders


def save_orders_jsonl(orders: List[Dict[str, Any]], output_path: Path) -> None:
    """Збереження у форматі JSONL (один JSON-об'єкт на рядок)."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as file:
        for order in orders:
            file.write(json.dumps(order, ensure_ascii=False) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Pizzeria Order Generator (JSONL)")
    parser.add_argument("--recipes", type=Path, default=Path(__file__).parent / "data" / "recipes.csv")
    parser.add_argument("--toppings", type=Path, default=Path(__file__).parent / "data" / "toppings.csv")
    parser.add_argument("--output", type=Path, required=True, help="Шлях для збереження .jsonl файлу")
    parser.add_argument("--count", type=int, default=100, help="Кількість замовлень на день")
    parser.add_argument("--seed", type=int, default=None, help="Random seed для відтворюваності")

    args = parser.parse_args()

    orders = generate_orders(
        recipes=args.recipes,
        toppings=args.toppings,
        count=args.count,
        seed=args.seed
    )
    save_orders_jsonl(orders, args.output)
    print(f"Успішно згенеровано {len(orders)} замовлень у файл: {args.output.resolve()}")


if __name__ == "__main__":
    main()
