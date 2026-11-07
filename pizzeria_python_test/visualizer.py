"""Tkinter: параметры запусков, прогресс и графики Matplotlib."""

import queue
import threading
from collections import Counter
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


class TestWindow:
    def __init__(self, settings: dict, run_experiments, validate_settings, generated_path):
        self.run_experiments = run_experiments
        self.validate_settings = validate_settings
        self.generated_path = Path(generated_path)
        self.root = tk.Tk()
        self.root.title("Pizzeria — эксперименты C++ / Python")
        self.root.geometry("950x720")
        self.root.minsize(800, 600)
        self.variables = {k: tk.StringVar(value=str(v)) for k, v in settings.items()}
        self.events = queue.Queue()
        self.stop_event = threading.Event()
        self.running = False
        self.results = []
        self.last_orders = []
        self.issue_counter = Counter()
        self.generation = ""
        self.make_widgets()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self._pump_id = self.root.after(100, self.pump)

    def make_widgets(self):
        frame = ttk.Frame(self.root, padding=14)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Параметры эксперимента", font=("Segoe UI", 16, "bold")).pack(anchor="w", pady=(0, 8))
        form = ttk.Frame(frame)
        form.pack(fill="x")
        fields = [
            ("threads", "Потоков C++"), ("runs", "Прогонов / итерацию"),
            ("generations", "Итераций оптимизации"), ("orders", "Заказов / итерацию"),
            ("max_ovens", "Доступно печей"), ("seed", "Seed"),
            ("rush_fraction", "Доля заказов в час пик (0–1)"),
        ]
        for i, (key, title) in enumerate(fields):
            r, c = divmod(i, 2)
            ttk.Label(form, text=title).grid(row=r, column=c * 2, sticky="w", padx=(0, 8), pady=4)
            ttk.Entry(form, width=15, textvariable=self.variables[key]).grid(row=r, column=c * 2 + 1,
                                                                              sticky="ew", padx=(0, 20), pady=4)
        ttk.Label(form, text="Алгоритм очереди").grid(row=4, column=0, sticky="w", pady=4)
        ttk.Combobox(form, state="readonly", textvariable=self.variables["mode"],
                     values=("fast", "bounded"), width=13).grid(row=4, column=1, sticky="w")
        form.columnconfigure(1, weight=1)
        form.columnconfigure(3, weight=1)

        files = ttk.LabelFrame(frame, text="Файлы", padding=8)
        files.pack(fill="x", pady=9)
        for i, (key, title) in enumerate((
            ("exe", "C++ .exe"), ("recipes", "Пиццы CSV"),
            ("toppings", "Топпинги CSV"), ("radius_wait", "Время ожидания CSV"),
        )):
            ttk.Label(files, text=title, width=19).grid(row=i, column=0, sticky="w", pady=3)
            ttk.Entry(files, textvariable=self.variables[key]).grid(row=i, column=1, sticky="ew", padx=5)
            ttk.Button(files, text="Обзор", command=lambda k=key: self.browse(k)).grid(row=i, column=2)
        files.columnconfigure(1, weight=1)

        controls = ttk.Frame(frame)
        controls.pack(fill="x", pady=(4, 10))
        self.start_button = ttk.Button(controls, text="Запустить эксперименты", command=self.start)
        self.start_button.pack(side="left")
        self.stop_button = ttk.Button(controls, text="Остановить", state="disabled", command=self.stop)
        self.stop_button.pack(side="left", padx=8)
        self.plot_button = ttk.Button(controls, text="Показать графики", state="disabled", command=self.show_plots)
        self.plot_button.pack(side="left")
        self.progress = ttk.Progressbar(frame, mode="determinate", maximum=100)
        self.progress.pack(fill="x")
        self.status_var = tk.StringVar(value="Готов к запуску")
        self.errors_var = tk.StringVar(value="Уведомления: зелёные 0 | жёлтые 0 | красные 0")
        ttk.Label(frame, textvariable=self.status_var).pack(anchor="w", pady=(5, 1))
        ttk.Label(frame, textvariable=self.errors_var).pack(anchor="w", pady=(1, 8))
        log_frame = ttk.LabelFrame(frame, text="Журнал выполнения", padding=5)
        log_frame.pack(fill="both", expand=True)
        log_controls = ttk.Frame(log_frame)
        log_controls.pack(fill="x", pady=(0, 4))
        ttk.Button(log_controls, text="Копировать всё", command=self.copy_log).pack(side="left")
        ttk.Button(log_controls, text="Выделить всё", command=self.select_log).pack(side="left", padx=6)
        ttk.Button(log_controls, text="Очистить журнал", command=self.clear_log).pack(side="left")
        self.log_text = tk.Text(log_frame, height=10, wrap="word", state="disabled", exportselection=False)
        self.log_text.bind("<Control-a>", self.select_log)
        self.log_text.bind("<Control-A>", self.select_log)
        self.log_text.bind("<Control-c>", self.copy_selection)
        self.log_text.bind("<Control-C>", self.copy_selection)
        self.log_text.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scrollbar.pack(side="right", fill="y")
        self.log_text.configure(yscrollcommand=scrollbar.set)

    def browse(self, key):
        path = filedialog.askopenfilename(title="Выбери файл", filetypes=[
            ("Executable", "*.exe"), ("CSV", "*.csv"), ("Все файлы", "*.*")])
        if path:
            self.variables[key].set(path)

    def write_log(self, line):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", str(line) + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log_text.get("1.0", "end-1c"))
        self.root.update_idletasks()

    def select_log(self, event=None):
        self.log_text.tag_add("sel", "1.0", "end-1c")
        self.log_text.focus_set()
        return "break"

    def copy_selection(self, event=None):
        try:
            selected = self.log_text.get("sel.first", "sel.last")
        except tk.TclError:
            return "break"
        self.root.clipboard_clear()
        self.root.clipboard_append(selected)
        self.root.update_idletasks()
        return "break"

    def clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def start(self):
        if self.running:
            return
        settings = {k: v.get().strip() for k, v in self.variables.items()}
        # Проверка до старта потока, ошибки показываем в окне.
        try:
            settings = self.validate_settings(settings)
        except (ValueError, OSError) as error:
            messagebox.showerror("Неверные настройки", str(error))
            return
        self.results.clear()
        self.last_orders = []
        self.issue_counter.clear()
        self.errors_var.set("Уведомления: зелёные 0 | жёлтые 0 | красные 0")
        self.stop_event = threading.Event()
        self.running = True
        self.start_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.plot_button.configure(state="disabled")
        self.progress["value"] = 0
        self.status_var.set("Запускаем...")

        def worker():
            try:
                def send(kind, payload):
                    self.events.put((kind, payload))
                rows, orders = self.run_experiments(settings, send, self.stop_event)
                self.events.put(("done", (rows, orders)))
            except Exception as exc:
                self.events.put(("failure", str(exc)))
        # Очередь содержит только данные. Tkinter вызывается исключительно из основного потока.
        threading.Thread(target=worker, daemon=True).start()

    def stop(self):
        self.stop_event.set()
        self.status_var.set("Запрошена отмена: ожидаем завершения C++...")

    def pump(self):
        try:
            while True:
                kind, data = self.events.get_nowait()
                if kind == "log":
                    self.write_log(data)
                elif kind == "generation":
                    self.generation = f"Итерация {data[0]}/{data[1]}"
                    self.write_log("\n" + "=" * 53)
                    self.write_log(f"НАЧАЛО {self.generation.upper()} — {data[2]}")
                    self.write_log("=" * 53)
                    self.progress["value"] = 0
                    self.status_var.set(self.generation + " — подготовка")
                elif kind == "progress":
                    finished, total, percent = data
                    self.progress["value"] = percent
                    self.status_var.set(f"{self.generation}: готово {finished}/{total}; "
                                        f"оценка выполнения ~{percent:.1f}%")
                elif kind == "result":
                    self.issue_counter.update({i: data["issues"][i - 1] for i in (1, 2, 3)})
                    self.errors_var.set(
                        f"Уведомления: зелёные {self.issue_counter[1]} | "
                        f"жёлтые {self.issue_counter[2]} | красные {self.issue_counter[3]}")
                    self.write_log(f"{data['id']}: {data['status']}, прибыль {data['profit']:.2f}, "
                                   f"проблемы 1/2/3: {data['issues']}")
                elif kind == "done":
                    self.results, self.last_orders = data
                    self.finished()
                elif kind == "failure":
                    self.write_log("Ошибка: " + data)
                    self.finished()
                    messagebox.showerror("Ошибка теста", data)
        except queue.Empty:
            pass
        self._pump_id = self.root.after(100, self.pump)

    def finished(self):
        self.running = False
        self.start_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        self.plot_button.configure(state="normal" if self.results else "disabled")
        from analyzer import summarize
        total = summarize(self.results)
        self.status_var.set(f"Завершено: успешных {total['completed']}/{total['total']}; "
                            f"суммарная прибыль {total['total_profit']:.2f}")
        if self.results:
            self.show_plots()

    def show_plots(self):
        try:
            draw_plots(self.results, self.last_orders, self.generated_path)
        except Exception as exc:
            messagebox.showerror("Графики", f"Не удалось открыть графики: {exc}\n"
                                 "Установи библиотеки: python -m pip install numpy matplotlib")

    def close(self):
        if self.running:
            self.stop()
            messagebox.showinfo("Остановка", "Запрошена остановка. Закрой окно после завершения C++.")
        else:
            if self._pump_id is not None:
                self.root.after_cancel(self._pump_id)
                self._pump_id = None
            self.root.destroy()

    def run(self):
        self.root.mainloop()


def draw_plots(rows: list, orders: list, generated_path: Path):
    """Окна Matplotlib, вызываются только из Tk-потока."""
    import numpy as np
    import matplotlib.pyplot as plt

    if not rows:
        return
    names = [r["id"].split("_")[-1] + "/" + r["id"].split("_")[-2]
             if len(r["id"].split("_")) >= 2 else r["id"] for r in rows]
    x = np.arange(len(rows))
    profits = np.array([v["profit"] for v in rows])
    avg_ovens = np.array([v["avg_active_ovens"] for v in rows])
    issues = np.array([v["issues"] for v in rows])
    done = np.array([v["fulfilled"] for v in rows])
    declined = np.array([v["rejected"] for v in rows])

    fig, axs = plt.subplots(2, 2, figsize=(13, 8))
    fig.suptitle("Pizzeria: результаты моделирования", fontsize=15)
    axs[0, 0].bar(x, profits, color=["#3b82f6" if r["status"] == "completed" else "#ef4444" for r in rows])
    axs[0, 0].set_title("Прибыль по прогонам")
    axs[0, 0].set_ylabel("Прибыль")
    axs[0, 0].set_xticks(x)
    axs[0, 0].set_xticklabels(names, rotation=45, ha="right", fontsize=7)

    axs[0, 1].scatter(avg_ovens, profits, s=55, color="#2563eb", alpha=0.8)
    axs[0, 1].set_title("Прибыль и среднее число активных печей")
    axs[0, 1].set_xlabel("Среднее число активных печей за смену")
    axs[0, 1].set_ylabel("Прибыль")
    axs[0, 1].grid(alpha=0.2)

    bottom = np.zeros(len(rows))
    for i, (label, color) in enumerate((("Уровень 1", "#22c55e"),
                                        ("Уровень 2", "#eab308"),
                                        ("Уровень 3", "#ef4444"))):
        axs[1, 0].bar(x, issues[:, i], bottom=bottom, color=color, label=label)
        bottom += issues[:, i]
    axs[1, 0].set_title("Уведомления по уровням")
    axs[1, 0].legend(fontsize=8)
    axs[1, 0].set_xticks(x)
    axs[1, 0].set_xticklabels(names, rotation=45, ha="right", fontsize=7)

    axs[1, 1].bar(x, done, color="#22c55e", label="Выполнено")
    axs[1, 1].bar(x, declined, bottom=done, color="#fb923c", label="Отказано")
    axs[1, 1].set_title("Заказы: выполнено / отказано")
    axs[1, 1].set_xticks(x)
    axs[1, 1].set_xticklabels(names, rotation=45, ha="right", fontsize=7)
    axs[1, 1].legend(fontsize=8)
    fig.tight_layout()

    fig2, ax = plt.subplots(figsize=(10, 4))
    times = [order["timestamp_sec"] / 3600 for order in orders]
    ax.hist(times, bins=24, color="#6366f1", edgecolor="white")
    ax.set_title("Распределение входящих заказов (последняя итерация)")
    ax.set_xlabel("Часы от начала смены")
    ax.set_ylabel("Количество заказов")
    fig2.tight_layout()

    plots = Path(generated_path) / "plots"
    plots.mkdir(parents=True, exist_ok=True)
    marker = datetime.now().strftime("%Y%m%d_%H%M%S")
    fig.savefig(plots / f"dashboard_{marker}_NOFORGIT.png", dpi=140)
    fig2.savefig(plots / f"arrivals_{marker}_NOFORGIT.png", dpi=140)
    plt.show(block=False)


def launch_gui(settings: dict, run_experiments, validate_settings, generated_path):
    TestWindow(settings, run_experiments, validate_settings, generated_path).run()
