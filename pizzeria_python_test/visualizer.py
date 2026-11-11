"""
Модуль візуалізації результатів.
Будує та зберігає графіки на основі агрегованих даних Монте-Карло.
"""

from pathlib import Path
import matplotlib.pyplot as plt
from analyzer import analyze_all_reports


def plot_results(output_dir: Path, save_dir: Path = Path("images")) -> None:
    save_dir.mkdir(parents=True, exist_ok=True)

    if not output_dir.exists():
        print("Папка з результатами відсутня.")
        return

    data = analyze_all_reports(output_dir)

    if not data:
        print("Немає даних для візуалізації.")
        return

    ovens = [d["active_ovens"] for d in data]
    profits = [d["avg_profit"] for d in data]
    sim_p_loss = [d["avg_p_loss"] for d in data]
    theory_p_loss = [d["theory_p_loss"] for d in data]

    # 1. Графік прибутку
    plt.figure(figsize=(8, 5))
    plt.plot(ovens, profits, marker='o', color='b', linewidth=2, label='Сер. прибуток (грн, MC)')
    plt.title('Залежність чистого прибутку від кількості активних печей (Монте-Карло)')
    plt.xlabel('Кількість печей')
    plt.ylabel('Прибуток (грн)')
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(save_dir / "profit_vs_ovens.png", dpi=300)
    plt.close()

    # 2. Графік % відмов
    plt.figure(figsize=(8, 5))
    plt.plot(ovens, sim_p_loss, marker='s', color='r', linewidth=2, label='Імітація (% відмов, MC)')
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
