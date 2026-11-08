"""
Модуль візуалізації результатів.
Будує та зберігає графіки залежності прибутку від кількості печей та порівняння з теорією.
"""

from pathlib import Path
import matplotlib.pyplot as plt
from analyzer import analyze_run_vs_theory


def plot_results(output_dir: Path, save_dir: Path = Path("images")) -> None:
    """Будує та зберігає графіки аналізу."""
    save_dir.mkdir(parents=True, exist_ok=True)
    json_files = list(output_dir.glob("*.json"))

    if not json_files:
        print("Немає JSON-звітів для візуалізації.")
        return

    data = [analyze_run_vs_theory(f) for f in json_files]
    data.sort(key=lambda x: x["active_ovens"])

    ovens = [d["active_ovens"] for d in data]
    profits = [d["simulation"]["profit_uah"] for d in data]
    sim_p_loss = [d["simulation"]["p_loss_percent"] for d in data]
    theory_p_loss = [d["theory"]["p_loss_percent"] for d in data]

    # 1. Графік: Чистий прибуток vs Кількість печей
    plt.figure(figsize=(8, 5))
    plt.plot(ovens, profits, marker='o', color='b', linewidth=2, label='Чистий прибуток (грн)')
    plt.title('Залежність чистого прибутку від кількості активних печей')
    plt.xlabel('Кількість печей')
    plt.ylabel('Прибуток (грн)')
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(save_dir / "profit_vs_ovens.png", dpi=300)
    plt.close()

    # 2. Графік: Порівняння % відмов (Імітація vs Теорія)
    plt.figure(figsize=(8, 5))
    plt.plot(ovens, sim_p_loss, marker='s', color='r', linewidth=2, label='Імітація (% відмов)')
    plt.plot(ovens, theory_p_loss, marker='^', color='g', linestyle='--', linewidth=2, label='Теорія (% відмов)')
    plt.title('Порівняння відсотка відмов: Імітація vs Теорія СМО')
    plt.xlabel('Кількість печей')
    plt.ylabel('Відсоток відмов (%)')
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(save_dir / "p_loss_comparison.png", dpi=300)
    plt.close()

    print(f"Графіки успішно збережено в папку: {save_dir.resolve()}")


if __name__ == "__main__":
    plot_results(Path("output"))
