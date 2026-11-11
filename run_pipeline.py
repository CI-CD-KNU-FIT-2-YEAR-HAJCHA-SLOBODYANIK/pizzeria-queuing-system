import os
import sys
import json
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
from pathlib import Path

# Перевірка наявності Pillow для масштабування графіків
try:
    from PIL import Image, ImageTk
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

class PizzeriaPipelineGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Pizzeria Simulation Manager (Учасник 2)")
        self.root.geometry("1100x780")
        self.root.minsize(900, 600)

        self.img_profit_tk = None
        self.img_loss_tk = None
        self.log_stats = {"green": 0, "yellow": 0, "red": 0}

        self.style = ttk.Style()
        self.style.theme_use('clam')

        self._create_widgets()
        self.root.bind("<Configure>", self._on_resize)

    def _create_widgets(self):
        # 1. Секція вводу параметрів
        param_frame = ttk.LabelFrame(self.root, text=" Параметри експерименту ", padding=10)
        param_frame.pack(fill="x", padx=15, pady=10)

        # Рядок 0: Замовлення, Seed, Печі
        ttk.Label(param_frame, text="Замовлення:").grid(row=0, column=0, sticky="w", padx=5, pady=5)
        self.ent_orders = ttk.Entry(param_frame, width=8)
        self.ent_orders.insert(0, "500")
        self.ent_orders.grid(row=0, column=1, padx=5, pady=5)

        ttk.Label(param_frame, text="Seed:").grid(row=0, column=2, sticky="w", padx=5, pady=5)
        self.ent_seed = ttk.Entry(param_frame, width=8)
        self.ent_seed.insert(0, "10")
        self.ent_seed.grid(row=0, column=3, padx=5, pady=5)

        ttk.Label(param_frame, text="Печі (через пробіл):").grid(row=0, column=4, sticky="w", padx=5, pady=5)
        self.ent_ovens = ttk.Entry(param_frame, width=20)
        self.ent_ovens.insert(0, "2 3 4 5 6 7 8 9 10")
        self.ent_ovens.grid(row=0, column=5, padx=5, pady=5)

        # Рядок 1: Потоки, Режим черги, Кнопка запуску
        ttk.Label(param_frame, text="Потоки:").grid(row=1, column=0, sticky="w", padx=5, pady=5)
        self.ent_threads = ttk.Entry(param_frame, width=8)
        self.ent_threads.insert(0, "4")
        self.ent_threads.grid(row=1, column=1, padx=5, pady=5)

        ttk.Label(param_frame, text="Режим черги:").grid(row=1, column=2, sticky="w", padx=5, pady=5)
        self.combo_queue = ttk.Combobox(
            param_frame,
            values=["bounded", "fast", "lite-brute", "enclave-tree"],
            state="readonly",
            width=13
        )
        self.combo_queue.set("bounded")
        self.combo_queue.grid(row=1, column=3, padx=5, pady=5)

        self.btn_run = ttk.Button(param_frame, text="Запустити пайплайн", command=self.start_pipeline_thread)
        self.btn_run.grid(row=1, column=4, columnspan=2, padx=15, pady=5, sticky="ew")

        # 2. Індикатор прогресу та логи Учасника 1
        progress_frame = ttk.Frame(self.root, padding=10)
        progress_frame.pack(fill="x", padx=15)

        top_status_frame = ttk.Frame(progress_frame)
        top_status_frame.pack(fill="x", pady=(0, 5))

        self.lbl_status = ttk.Label(top_status_frame, text="Статус: Готовий до запуску", font=("Segoe UI", 10, "bold"))
        self.lbl_status.pack(side="left")

        self.lbl_logs_stats = ttk.Label(
            top_status_frame,
            text="Зелені: 0  |  Жовті: 0  |  Червоні: 0",
            font=("Segoe UI", 9, "bold"),
            foreground="#333333"
        )
        self.lbl_logs_stats.pack(side="right")

        self.progress_bar = ttk.Progressbar(progress_frame, mode="determinate", maximum=100)
        self.progress_bar.pack(fill="x")

        # 3. Вкладки (Логи / Графіки)
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=15, pady=10)

        # Вкладка 1: Консоль логів
        tab_logs = ttk.Frame(self.notebook)
        self.notebook.add(tab_logs, text="Консоль та Звіти")

        self.txt_log = scrolledtext.ScrolledText(tab_logs, wrap=tk.WORD, font=("Consolas", 9), bg="#1e1e1e", fg="#d4d4d4")
        self.txt_log.pack(fill="both", expand=True, padx=5, pady=5)

        # Вкладка 2: Графік Прибутку
        self.tab_profit = ttk.Frame(self.notebook)
        self.notebook.add(self.tab_profit, text="Прибуток")
        self.lbl_img_profit = ttk.Label(self.tab_profit, text="Запустіть симуляцію для генерації графіків", anchor="center")
        self.lbl_img_profit.pack(fill="both", expand=True)

        # Вкладка 3: Графік Відмов
        self.tab_loss = ttk.Frame(self.notebook)
        self.notebook.add(self.tab_loss, text="Відсоток відмов")
        self.lbl_img_loss = ttk.Label(self.tab_loss, text="Запустіть симуляцію для генерації графіків", anchor="center")
        self.lbl_img_loss.pack(fill="both", expand=True)

    def log(self, message):
        def _append():
            self.txt_log.insert(tk.END, message + "\n")
            self.txt_log.see(tk.END)
        self.root.after(0, _append)

    def set_status(self, text, progress_val=None):
        def _update():
            self.lbl_status.config(text=f"Статус: {text}")
            if progress_val is not None:
                self.progress_bar["value"] = progress_val
        self.root.after(0, _update)

    def update_log_counters(self, green, yellow, red):
        self.log_stats["green"] = green
        self.log_stats["yellow"] = yellow
        self.log_stats["red"] = red
        def _update():
            self.lbl_logs_stats.config(
                text=f"Зелені: {self.log_stats['green']}  |  Жовті: {self.log_stats['yellow']}  |  Червоні: {self.log_stats['red']}"
            )
        self.root.after(0, _update)

    def build_manifest(self, oven_counts, parallelism, queue_mode):
        return {
            "version": 1,
            "parallelism": parallelism,
            "max_ovens": max(oven_counts),
            "output_dir": "output",
            "include_oven_events": True,
            "tables_id_is_index": False,
            "queue": {
                "mode": queue_mode,
                "max_search_nodes": 600,
                "max_repack_nodes": 3000,
                "max_branch_per_task": 20,
                "max_variants": 16,
                "max_time_sec": 86400
            },
            "shared": {
                "recipes": "pizzeria_python_test/data/recipes.csv",
                "toppings": "pizzeria_python_test/data/toppings.csv",
                "orders": "pizzeria_python_test/data/orders.jsonl",
                "config": {
                    "opening_time": 32400, "closing_time": 79200,
                    "ingredient_price_g": {"meat": 0.3, "dough": 0.05, "cheese": 0.2, "veg": 0.1, "seafood": 0.4},
                    "oven_cost_sec": 0.0166, "idle_power_ratio": 0.5, "heat_up_sec": 300, "cool_down_sec": 300, "oven_capacity": 1,
                    "radius_wait_map": [{"radius_cm": 30, "wait_time": 600}, {"radius_cm": 42, "wait_time": 900}],
                    "online_delay_sec": 300, "in_person_delay_sec": 120, "quant_step_min": 5, "quant_by_clock": True,
                    "max_early_prep_sec": 1200, "soft_delay_limit_sec": 300, "dynamic_policy": []
                }
            },
            "runs": [{"id": f"run_{c}_ovens", "max_ovens": c, "config": {"static_policy": [{"time": 0, "target_ovens": c}]}} for c in oven_counts]
        }

    def start_pipeline_thread(self):
        try:
            orders_count = int(self.ent_orders.get().strip())
            seed_val = int(self.ent_seed.get().strip())
            oven_counts = sorted(list(set(int(x) for x in self.ent_ovens.get().strip().split())))
            parallelism = int(self.ent_threads.get().strip())
            queue_mode = self.combo_queue.get().strip()
        except ValueError:
            messagebox.showerror("Помилка вводу", "Перевірте правильність введених чисел!")
            return

        self.btn_run.config(state="disabled")
        self.txt_log.delete("1.0", tk.END)
        self.update_log_counters(0, 0, 0)

        threading.Thread(target=self._run_pipeline, args=(orders_count, seed_val, oven_counts, parallelism, queue_mode), daemon=True).start()

    def _run_pipeline(self, orders_count, seed_val, oven_counts, parallelism, queue_mode):
        try:
            out_dir = Path("output")
            out_dir.mkdir(parents=True, exist_ok=True)

            # 1. Очищення
            self.set_status("Очищення попередніх звітів...", 10)
            self.log("[1/5] Очищення папки output/...")
            for f in out_dir.rglob("*.json"):
                try:
                    f.unlink()
                except OSError:
                    pass

            # 2. Генерація
            self.set_status("Генерація замовлень...", 25)
            self.log(f"[2/5] Генерація {orders_count} замовлень (seed: {seed_val})...")
            gen_res = subprocess.run([sys.executable, "pizzeria_python_test/generator.py",
                                      "--output", "pizzeria_python_test/data/orders.jsonl",
                                      "--count", str(orders_count), "--seed", str(seed_val)],
                                     capture_output=True, text=True)
            if gen_res.stdout: self.log(gen_res.stdout)

            # 3. Маніфест
            self.set_status("Збірка manifest.json...", 40)
            self.log(f"[3/5] Створення manifest.json (потоки: {parallelism}, режим черги: {queue_mode})...")
            manifest_data = self.build_manifest(oven_counts, parallelism, queue_mode)
            with open("manifest.json", "w", encoding="utf-8") as f:
                json.dump(manifest_data, f, indent=2)

            # 4. Пошук та запуск C++ двигуна
            self.set_status("Виконання C++ симуляції...", 60)

            possible_paths = [
                Path("simulation_engine.exe"),
                Path("x64/Release/simulation_engine.exe"),
                Path("x64/Debug/simulation_engine.exe"),
                Path("build/simulation_engine.exe"),
                Path("simulation_engine/x64/Debug/simulation_engine.exe"),
                Path("out/build/x64-Debug/simulation_engine.exe")
            ]

            engine_path = None
            for p in possible_paths:
                if p.exists():
                    engine_path = p
                    break

            if not engine_path:
                raise FileNotFoundError(
                    "Не знайдено simulation_engine.exe! Перевірте наявність файлу в корені або папці збірки."
                )

            self.log(f"[4/5] Запуск C++ двигуна ({engine_path}) для конфігурацій: {oven_counts}...")
            engine_cmd = [str(engine_path), "--manifest", "manifest.json", "--overwrite"]

            process = subprocess.Popen(engine_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
            for line in process.stdout:
                line_str = line.strip()
                if not line_str: continue
                try:
                    data = json.loads(line_str)
                    if data.get("event") == "finished":
                        self.log(f"  Прогін {data.get('run_id')} завершено.")
                except json.JSONDecodeError:
                    self.log(f"  [C++] {line_str}")

            process.wait()
            if process.returncode != 0:
                self.log(f"  Увага: C++ двигун завершився з кодом {process.returncode}")

            # Парсинг рекурсивно усіх JSON-звітів у папці output/
            green_cnt, yellow_cnt, red_cnt = 0, 0, 0
            json_files = list(out_dir.rglob("*.json"))
            for report_file in json_files:
                if report_file.name.startswith("analysis_") or report_file.name.startswith("manifest_"):
                    continue
                try:
                    with report_file.open("r", encoding="utf-8-sig") as f:
                        rep_data = json.load(f)
                    raw_notifs = rep_data.get("results", {}).get("notifications", [])
                    for row in raw_notifs:
                        if not isinstance(row, dict): continue
                        sev = int(row.get("severity", 0))
                        if sev == 1:
                            green_cnt += 1
                        elif sev == 2:
                            yellow_cnt += 1
                        elif sev == 3:
                            red_cnt += 1
                except Exception:
                    pass

            self.update_log_counters(green_cnt, yellow_cnt, red_cnt)
            self.log(f"\nЗнайдено JSON-звітів: {len(json_files)}")
            self.log(f"Сповіщення з логів JSON: Зелені: {green_cnt} | Жовті: {yellow_cnt} | Червоні: {red_cnt}")

            # 5. Аналіз та Візуалізація
            self.set_status("Аналіз результатів та генерація графіків...", 85)
            self.log("\n[5/5] Виконання аналізу та побудова графіків...")

            ana_res = subprocess.run([sys.executable, "pizzeria_python_test/analyzer.py"], capture_output=True, text=True)
            if ana_res.stdout: self.log("\n--- ЗВІТ АНАЛІЗАТОРА ---\n" + ana_res.stdout)
            if ana_res.stderr: self.log(ana_res.stderr)

            subprocess.run([sys.executable, "pizzeria_python_test/visualizer.py"], capture_output=True, text=True)

            self.set_status("Завершено успішно!", 100)
            self.log("\nВсі операції успішно виконані!")

            self.root.after(0, self._load_generated_images)

        except Exception as e:
            self.log(f"\nВиникла помилка: {str(e)}")
            self.set_status("Помилка виконання!", 0)
        finally:
            self.root.after(0, lambda: self.btn_run.config(state="normal"))

    def _load_generated_images(self):
        img_profit_path = Path("images/profit_vs_ovens.png")
        img_loss_path = Path("images/p_loss_comparison.png")

        tab_w = max(self.notebook.winfo_width() - 30, 600)
        tab_h = max(self.notebook.winfo_height() - 40, 400)

        if HAS_PIL:
            if img_profit_path.exists():
                img = Image.open(img_profit_path)
                img.thumbnail((tab_w, tab_h), Image.Resampling.LANCZOS)
                self.img_profit_tk = ImageTk.PhotoImage(img)
                self.lbl_img_profit.config(image=self.img_profit_tk, text="")

            if img_loss_path.exists():
                img = Image.open(img_loss_path)
                img.thumbnail((tab_w, tab_h), Image.Resampling.LANCZOS)
                self.img_loss_tk = ImageTk.PhotoImage(img)
                self.lbl_img_loss.config(image=self.img_loss_tk, text="")
        else:
            if img_profit_path.exists():
                self.img_profit_tk = tk.PhotoImage(file=str(img_profit_path)).subsample(2, 2)
                self.lbl_img_profit.config(image=self.img_profit_tk, text="")
            if img_loss_path.exists():
                self.img_loss_tk = tk.PhotoImage(file=str(img_loss_path)).subsample(2, 2)
                self.lbl_img_loss.config(image=self.img_loss_tk, text="")

    def _on_resize(self, event):
        if event.widget == self.root and (hasattr(self, 'img_profit_tk') and self.img_profit_tk):
            self._load_generated_images()

if __name__ == "__main__":
    root = tk.Tk()
    app = PizzeriaPipelineGUI(root)
    root.mainloop()
