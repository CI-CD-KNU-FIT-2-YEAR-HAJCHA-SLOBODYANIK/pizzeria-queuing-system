"""Переглядач сповіщень і пікової пам'яті C++-симулятора піцерії.

Запуск: python debug_logs.py [папка_звітів | звіт.json]
Потрібен лише Python із Tkinter. Вхідні JSON-файли не змінюються.
"""

from __future__ import annotations

import csv
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


SEVERITY_NAMES = {
    0: "0 — Інші",
    1: "1 — Рекомендації",
    2: "2 — Попередження",
    3: "3 — Критичні",
}
SEVERITY_COLORS = {1: "#187a43", 2: "#ad7400", 3: "#ba3030"}
CHECKED, UNCHECKED = "☑", "☐"
RUN_PATTERN = re.compile(r"^(?P<session>\d{8}_\d{4})_g(?P<generation>\d+)_r\d+$")
OVENS_PATTERN = re.compile(r"(?:^|_)run_(\d+)_ovens$|^(\d+)_ovens$", re.IGNORECASE)


def number(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def memory_size(value: int) -> str:
    if value >= 1024 ** 3:
        return f"{value / 1024 ** 3:,.2f} ГіБ"
    if value >= 1024 ** 2:
        return f"{value / 1024 ** 2:,.2f} МіБ"
    if value >= 1024:
        return f"{value / 1024:,.2f} КіБ"
    return f"{value:,} Б"


def clock_time(value: int) -> str:
    if value < 0:
        return "—"
    h, rem = divmod(value, 3600)
    m, s = divmod(rem, 60)
    return f"{h:02d}:{m:02d}:{s:02d}"


@dataclass(frozen=True)
class Notice:
    run_id: str
    generation: str
    level: int
    code: str
    timestamp: object
    thread_id: object
    message: str
    recommendation: str
    source: Path


@dataclass(frozen=True)
class MemoryStat:
    # "peak" — найбільша оцінка за прогін; "last" — лише останній пошук.
    kind: str
    estimated_peak_bytes: int
    manager_peak_bytes: int
    search_peak_bytes: int
    searches_observed: int = -1
    peak_time_sec: int = -1
    queue_tasks_at_peak: int = -1
    max_queue_tasks: int = -1
    nodes_at_peak: int = -1
    enclaves_at_peak: int = -1
    budget_exhausted_at_peak: bool = False
    approximate: bool = True


@dataclass
class Run:
    run_id: str
    generation: str
    session: str
    status: str
    failure: str
    path: Path
    notices: list[Notice]
    memory: MemoryStat | None = None
    queue_mode: str = "—"
    ovens: int = -1


def read_memory(data: dict, notices: list[Notice]) -> MemoryStat | None:
    # Новий формат: пікова статистика є окремим об'єктом на верхньому рівні.
    raw = data.get("queue_memory_peak")
    if raw is None and isinstance(data.get("results"), dict):
        raw = data["results"].get("queue_memory_peak")
    if isinstance(raw, dict) and "estimated_peak_bytes" in raw:
        return MemoryStat(
            kind="peak",
            estimated_peak_bytes=max(0, number(raw.get("estimated_peak_bytes"))),
            manager_peak_bytes=max(0, number(raw.get("manager_peak_bytes"))),
            search_peak_bytes=max(0, number(raw.get("search_peak_bytes"))),
            searches_observed=number(raw.get("searches_observed"), -1),
            peak_time_sec=number(raw.get("peak_time_sec"), -1),
            queue_tasks_at_peak=number(raw.get("queue_tasks_at_peak"), -1),
            max_queue_tasks=number(raw.get("max_queue_tasks"), -1),
            nodes_at_peak=number(raw.get("nodes_at_peak"), -1),
            enclaves_at_peak=number(raw.get("enclaves_at_peak"), -1),
            budget_exhausted_at_peak=bool(raw.get("budget_exhausted_at_peak", False)),
            approximate=bool(raw.get("approximate", True)),
        )

    # Сумісність зі старими звітами. Повідомлення містить ОСТАННІЙ пошук,
    # тому значення не можна вважати піком за весь час симуляції.
    for notice in reversed(notices):
        if notice.code != "QUEUE_MEMORY_USAGE":
            continue
        fields = dict(re.findall(r"([a-z_]+)\s*=\s*([^;\s]+)", notice.message))
        if "estimated_peak_bytes" not in fields:
            continue
        return MemoryStat(
            kind="last",
            estimated_peak_bytes=max(0, number(fields.get("estimated_peak_bytes"))),
            manager_peak_bytes=max(0, number(fields.get("current_bytes"))),
            search_peak_bytes=max(0, number(fields.get("last_search_peak_bytes"))),
            peak_time_sec=number(notice.timestamp, -1),
            nodes_at_peak=number(fields.get("nodes"), -1),
            enclaves_at_peak=number(fields.get("enclaves"), -1),
            budget_exhausted_at_peak=bool(number(fields.get("budget_exhausted"))),
        )
    return None


def detect_folder(script_path: Path) -> Path:
    """Шукає папку зі звітами біля скрипту або з кореня проєкту."""
    candidates = []
    for base in (script_path.parent, Path.cwd()):
        for parent in [base, *list(base.parents)[:3]]:
            candidates.extend([
                parent / parent / "output",
            ])
    seen = set()
    available = []
    for path in candidates:
        if path in seen:
            continue
        seen.add(path)
        if path.is_dir():
            if any(path.rglob("*.json")):
                return path.resolve()
            available.append(path)
    return (available[0] if available else script_path.parent / "output").resolve()


def load_reports(location: Path) -> tuple[list[Run], list[str]]:
    """Читає прогони з output/, generated/reports/ або окремого JSON."""
    runs, errors = [], []
    if location.is_file() and location.suffix.lower() == ".json":
        paths = [location]
    elif location.is_dir():
        paths = sorted(location.rglob("*.json"))
    else:
        return [], [f"Папку або файл не знайдено: {location}"]

    for path in paths:
        if path.name.startswith(("analysis_", "manifest_")):
            continue
        try:
            with path.open("r", encoding="utf-8-sig") as stream:
                data = json.load(stream)
            if not isinstance(data, dict):
                continue
            results = data.get("results")
            if not isinstance(results, dict):
                if "run_id" not in data or "status" not in data:
                    continue
                results = {}
            rid = str(data.get("run_id") or path.stem)
            match = RUN_PATTERN.fullmatch(rid)
            parent_name = path.parent.name
            session = match.group("session") if match else parent_name
            generation = (f"{session} / g{int(match.group('generation')):03d}"
                          if match else f"{session} / звіти")
            raw_notices = results.get("notifications") or data.get("notifications") or []
            notices = []
            for row in raw_notices if isinstance(raw_notices, list) else []:
                if not isinstance(row, dict):
                    continue
                notices.append(Notice(
                    run_id=rid,
                    generation=generation,
                    level=number(row.get("severity")),
                    code=str(row.get("code") or "(без коду)"),
                    timestamp=row.get("timestamp_sec", "—"),
                    thread_id=row.get("thread_id", "—"),
                    message=str(row.get("message") or ""),
                    recommendation=str(row.get("recommendation") or ""),
                    source=path,
                ))
            failure = data.get("failure")
            if isinstance(failure, (dict, list)):
                failure = json.dumps(failure, ensure_ascii=False)
            config = data.get("config") or {}
            config = config if isinstance(config, dict) else {}
            queue_cfg = config.get("queue") or {}
            queue_cfg = queue_cfg if isinstance(queue_cfg, dict) else {}
            ovens = number(config.get("max_ovens"), -1)
            if ovens < 0:
                oven_match = OVENS_PATTERN.search(rid)
                if oven_match:
                    ovens = number(next(x for x in oven_match.groups() if x is not None), -1)
            runs.append(Run(
                run_id=rid,
                generation=generation,
                session=session,
                status=str(data.get("status", "unknown")),
                failure=str(failure or ""),
                path=path,
                notices=notices,
                memory=read_memory(data, notices),
                queue_mode=str(queue_cfg.get("mode") or "—"),
                ovens=ovens,
            ))
        except (OSError, UnicodeError, ValueError, TypeError) as exc:
            errors.append(f"{path}: {exc}")

    runs.sort(key=lambda run: (run.generation, run.ovens if run.ovens >= 0 else 10 ** 9, run.run_id, str(run.path)))
    return runs, errors


class DebugViewer(tk.Tk):
    def __init__(self, location: Path):
        super().__init__()
        self.title("Pizzeria — Сповіщення та пам'ять черги")
        self.geometry("1460x850")
        self.minsize(1050, 630)

        self.folder = location
        self.runs: list[Run] = []
        self.errors: list[str] = []
        self.scope_map: dict[str, tuple[str, str]] = {}
        self.notices: list[Notice] = []
        self.visible: list[Notice] = []
        self.selected_runs: list[Run] = []
        self.memory_rows: list[Run] = []
        self.code_enabled: dict[tuple[int, str], bool] = {}
        self.checkbox_map: dict[str, tuple[int, str | None]] = {}
        self.level_enabled = {level: True for level in SEVERITY_NAMES}
        self.memory_selected: str | None = None
        self.chart_hits: list[tuple[float, float, float, float, str]] = []
        self.find_text = tk.StringVar()
        self.folder_text = tk.StringVar(value=str(location))
        self.counter = tk.StringVar(value="Звіти ще не завантажено")
        self.memory_summary = tk.StringVar(value="")
        self.memory_hint = tk.StringVar(value="")
        self.memory_chart_status = tk.StringVar(value="")
        self.memory_mode = tk.StringVar(value="Піки за прогонами")

        self._build_ui()
        self.reload()
        self.bind_all("<Control-r>", lambda _event: self.reload())
        self.bind_all("<Control-f>", self._focus_search)

    def _focus_search(self, _event):
        self.notebook.select(self.notice_tab)
        self.search_entry.focus_set()
        return "break"

    def _build_ui(self):
        toolbar = ttk.Frame(self, padding=8)
        toolbar.pack(fill="x")
        ttk.Label(toolbar, text="Звіти:").pack(side="left")
        ttk.Entry(toolbar, textvariable=self.folder_text).pack(side="left", fill="x", expand=True, padx=7)
        ttk.Button(toolbar, text="Папка…", command=self.choose_folder).pack(side="left", padx=3)
        ttk.Button(toolbar, text="Відкрити JSON…", command=self.open_file).pack(side="left", padx=3)
        ttk.Button(toolbar, text="Оновити", command=self.reload).pack(side="left", padx=3)

        body = ttk.Panedwindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=8, pady=(0, 6))
        sidebar = ttk.Frame(body)
        body.add(sidebar, weight=3)
        ttk.Label(sidebar, text="Сесії / покоління / прогони").pack(anchor="w", pady=(0, 5))
        scopes_frame = ttk.Frame(sidebar)
        scopes_frame.pack(fill="both", expand=True)
        self.scopes = ttk.Treeview(scopes_frame, show="tree", selectmode="browse")
        self.scopes.pack(side="left", fill="both", expand=True)
        scopes_scroll = ttk.Scrollbar(scopes_frame, orient="vertical", command=self.scopes.yview)
        scopes_scroll.pack(side="right", fill="y")
        self.scopes.configure(yscrollcommand=scopes_scroll.set)
        self.scopes.bind("<<TreeviewSelect>>", lambda _event: self.scope_changed())
        ttk.Label(sidebar, text="Вибір застосовується до обох вкладок.", foreground="#64748b").pack(anchor="w", pady=5)

        right = ttk.Frame(body)
        body.add(right, weight=9)
        self.notebook = ttk.Notebook(right)
        self.notebook.pack(fill="both", expand=True)
        self.notice_tab = ttk.Frame(self.notebook)
        self.memory_tab = ttk.Frame(self.notebook)
        self.notebook.add(self.notice_tab, text="Сповіщення")
        self.notebook.add(self.memory_tab, text="Пам'ять черги")
        self._build_notice_tab()
        self._build_memory_tab()

        status = ttk.Frame(self, padding=(8, 0, 8, 8))
        status.pack(fill="x")
        ttk.Label(status, textvariable=self.counter).pack(side="left", fill="x", expand=True)
        ttk.Button(status, text="Експорт TXT…", command=self.export_txt).pack(side="right", padx=5)
        ttk.Button(status, text="Копіювати видимі сповіщення", command=lambda: self.copy_rows(all_rows=True)).pack(side="right")

    def _build_notice_tab(self):
        content = ttk.Panedwindow(self.notice_tab, orient="vertical")
        content.pack(fill="both", expand=True)

        filters_frame = ttk.Frame(content, padding=(5, 5))
        content.add(filters_frame, weight=2)
        ttk.Label(filters_frame, text="Рівні → коди сповіщень (натисніть рядок для зміни фільтра)").pack(anchor="w")
        treebox = ttk.Frame(filters_frame)
        treebox.pack(fill="both", expand=True, pady=4)
        self.filters = ttk.Treeview(treebox, columns=("count",), show="tree headings")
        self.filters.heading("#0", text="Фільтр")
        self.filters.heading("count", text="Кількість")
        self.filters.column("count", width=90, minwidth=70, anchor="e", stretch=False)
        self.filters.pack(side="left", fill="both", expand=True)
        yscroll = ttk.Scrollbar(treebox, orient="vertical", command=self.filters.yview)
        yscroll.pack(side="right", fill="y")
        self.filters.configure(yscrollcommand=yscroll.set)
        self.filters.bind("<ButtonRelease-1>", self.toggle_filter)
        buttons = ttk.Frame(filters_frame)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="Усі коди", command=lambda: self.set_all_filters(True)).pack(side="left")
        ttk.Button(buttons, text="Зняти всі", command=lambda: self.set_all_filters(False)).pack(side="left", padx=5)
        ttk.Button(buttons, text="Лише критичні", command=self.only_critical).pack(side="left")

        results = ttk.Frame(content, padding=(5, 3))
        content.add(results, weight=5)
        searchbar = ttk.Frame(results)
        searchbar.pack(fill="x", pady=(0, 4))
        ttk.Label(searchbar, text="Пошук за кодом / повідомленням:").pack(side="left")
        self.search_entry = ttk.Entry(searchbar, textvariable=self.find_text)
        self.search_entry.pack(side="left", expand=True, fill="x", padx=6)
        ttk.Button(searchbar, text="Копіювати вибране", command=self.copy_rows).pack(side="right")
        self.find_text.trace_add("write", lambda *_: self.update_results())

        gridframe = ttk.Frame(results)
        gridframe.pack(fill="both", expand=True)
        cols = ("run", "level", "code", "time", "thread", "message")
        self.table = ttk.Treeview(gridframe, columns=cols, show="headings", selectmode="extended")
        specs = [
            ("run", "Прогін", 185), ("level", "Рів.", 44), ("code", "Код", 185),
            ("time", "Сек.", 73), ("thread", "Потік", 60), ("message", "Повідомлення", 540),
        ]
        for column, title, width in specs:
            self.table.heading(column, text=title)
            self.table.column(column, width=width, minwidth=42, stretch=column == "message")
        for level, color in SEVERITY_COLORS.items():
            self.table.tag_configure(f"level{level}", foreground=color)
        self.table.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(gridframe, orient="vertical", command=self.table.yview)
        scroll.pack(side="right", fill="y")
        self.table.configure(yscrollcommand=scroll.set)
        self.table.bind("<<TreeviewSelect>>", lambda _event: self.show_detail())

        detail_frame = ttk.Frame(content, padding=(5, 3))
        content.add(detail_frame, weight=3)
        detailbar = ttk.Frame(detail_frame)
        detailbar.pack(fill="x")
        ttk.Label(detailbar, text="Подробиці вибраного сповіщення / стан прогонів").pack(side="left")
        ttk.Button(detailbar, text="Копіювати", command=self.copy_detail).pack(side="right")
        self.detail = tk.Text(detail_frame, height=8, wrap="word", font=("Consolas", 10), undo=False)
        self.detail.pack(fill="both", expand=True, pady=(4, 0))
        self.detail.configure(state="disabled")

    def _build_memory_tab(self):
        top = ttk.Frame(self.memory_tab, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="Пам'ять пошуку черги", font=("Segoe UI", 12, "bold")).pack(anchor="w")
        ttk.Label(top, textvariable=self.memory_summary, font=("Segoe UI", 11, "bold"),
                  wraplength=1100, justify="left").pack(anchor="w", pady=(3, 0))
        ttk.Label(top, textvariable=self.memory_hint, foreground="#64748b",
                  wraplength=1150, justify="left").pack(anchor="w", pady=(2, 5))

        actions = ttk.Frame(self.memory_tab, padding=(8, 0, 8, 5))
        actions.pack(fill="x")
        ttk.Label(actions, text="Графік:").pack(side="left")
        options = ttk.Combobox(actions, state="readonly", textvariable=self.memory_mode,
                               values=("Піки за прогонами", "Час появи піків"), width=23)
        options.pack(side="left", padx=8)
        options.bind("<<ComboboxSelected>>", lambda _event: self.draw_memory_chart())
        ttk.Button(actions, text="Експорт CSV…", command=self.export_memory_csv).pack(side="right", padx=4)
        ttk.Button(actions, text="Копіювати максимум", command=self.copy_maximum).pack(side="right", padx=4)

        chart_frame = ttk.Frame(self.memory_tab, padding=(6, 0, 6, 4))
        chart_frame.pack(fill="both", expand=True)
        self.memory_canvas = tk.Canvas(chart_frame, background="#ffffff", highlightthickness=1,
                                       highlightbackground="#d1d5db", height=300)
        self.memory_canvas.pack(side="top", fill="both", expand=True)
        self.memory_canvas.bind("<Configure>", lambda _event: self.draw_memory_chart())
        self.memory_canvas.bind("<Motion>", self.chart_motion)
        self.memory_canvas.bind("<Leave>", lambda _event: self.memory_chart_status.set(""))
        self.memory_canvas.bind("<Button-1>", self.chart_click)
        self.chart_scroll = ttk.Scrollbar(chart_frame, orient="horizontal", command=self.memory_canvas.xview)
        self.chart_scroll.pack(fill="x")
        self.memory_canvas.configure(xscrollcommand=self.chart_scroll.set)
        ttk.Label(self.memory_tab, textvariable=self.memory_chart_status, foreground="#334155",
                  padding=(9, 0, 9, 5)).pack(anchor="w")

        lower = ttk.Panedwindow(self.memory_tab, orient="vertical")
        lower.pack(fill="both", expand=True, padx=8, pady=(0, 7))
        table_frame = ttk.Frame(lower)
        lower.add(table_frame, weight=3)
        columns = ("run", "mode", "ovens", "total", "manager", "search", "time", "kind")
        self.memory_table = ttk.Treeview(table_frame, columns=columns, show="headings", height=7, selectmode="browse")
        headers = [
            ("run", "Прогін", 160), ("mode", "Алгоритм", 94), ("ovens", "Печі", 50),
            ("total", "Разом", 105), ("manager", "Менеджер", 105),
            ("search", "Пошук", 105), ("time", "Час піка", 85), ("kind", "Джерело", 115),
        ]
        for column, title, width in headers:
            self.memory_table.heading(column, text=title)
            self.memory_table.column(column, width=width, minwidth=45, stretch=column == "run")
        self.memory_table.tag_configure("peak", foreground="#0f766e")
        self.memory_table.tag_configure("legacy", foreground="#80601c")
        self.memory_table.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(table_frame, orient="vertical", command=self.memory_table.yview)
        scroll.pack(side="right", fill="y")
        self.memory_table.configure(yscrollcommand=scroll.set)
        self.memory_table.bind("<<TreeviewSelect>>", lambda _event: self.memory_row_selected())

        self.memory_detail = tk.Text(lower, height=6, wrap="word", font=("Consolas", 10), undo=False)
        self.memory_detail.configure(state="disabled")
        lower.add(self.memory_detail, weight=2)

    def choose_folder(self):
        initial = self.folder if self.folder.is_dir() else self.folder.parent
        path = filedialog.askdirectory(initialdir=initial if initial.exists() else Path.cwd(),
                                      title="Виберіть папку output або generated/reports")
        if path:
            self.folder_text.set(path)
            self.reload()

    def open_file(self):
        initial = self.folder if self.folder.is_dir() else self.folder.parent
        path = filedialog.askopenfilename(title="Відкрити JSON-звіт симуляції",
                                          filetypes=[("JSON", "*.json")],
                                          initialdir=initial if initial.exists() else Path.cwd())
        if path:
            self.folder_text.set(path)
            self.reload()

    def reload(self):
        raw = self.folder_text.get().strip()
        self.folder = Path(raw).expanduser().resolve() if raw else Path.cwd()
        self.runs, self.errors = load_reports(self.folder)
        selected = self.scopes.selection()[0] if self.scopes.selection() else "all"
        self.scopes.delete(*self.scopes.get_children())
        self.scope_map.clear()
        count = sum(len(run.notices) for run in self.runs)
        self.scopes.insert("", "end", iid="all",
                           text=f"Усі звіти ({len(self.runs)} прогонів, {count} сповіщень)", open=True)
        self.scope_map["all"] = ("all", "")
        by_session: dict[str, dict[str, list[Run]]] = {}
        for run in self.runs:
            by_session.setdefault(run.session, {}).setdefault(run.generation, []).append(run)
        serial = 0
        for session, generations in sorted(by_session.items(), reverse=True):
            sid = f"session:{session}"
            self.scopes.insert("all", "end", iid=sid,
                               text=f"Сесія {session} — {sum(map(len, generations.values()))} прогонів", open=True)
            self.scope_map[sid] = ("session", session)
            for generation, group in sorted(generations.items()):
                gid = f"gen:{generation}"
                self.scopes.insert(sid, "end", iid=gid,
                                   text=f"{generation.split(' / ')[-1]} — {len(group)} прогонів", open=True)
                self.scope_map[gid] = ("gen", generation)
                for run in group:
                    serial += 1
                    iid = f"run:{serial}"
                    desc = f"{run.run_id} — {run.status} ({len(run.notices)})"
                    self.scopes.insert(gid, "end", iid=iid, text=desc)
                    self.scope_map[iid] = ("run", str(run.path))
        if not self.scopes.exists(selected):
            selected = "all"
        self.scopes.selection_set(selected)
        self.scope_changed()
        if self.errors:
            messagebox.showwarning("Помилки читання звітів",
                                   f"Помилок: {len(self.errors)}\n\n" + "\n".join(self.errors[:5]))

    def scope_changed(self):
        selection = self.scopes.selection()
        kind, key = self.scope_map.get(selection[0] if selection else "all", ("all", ""))
        if kind == "all":
            chosen = self.runs
        elif kind == "session":
            chosen = [run for run in self.runs if run.session == key]
        elif kind == "gen":
            chosen = [run for run in self.runs if run.generation == key]
        else:
            chosen = [run for run in self.runs if str(run.path) == key]
        self.selected_runs = chosen
        self.notices = [notice for run in chosen for notice in run.notices]
        self._build_filter_tree()
        self.update_results()
        failures = [run for run in chosen if run.status != "completed"]
        info = (f"Прогонів: {len(chosen)}\n"
                f"Успішно завершено: {len(chosen) - len(failures)}\n"
                f"Неуспішних / незавершених: {len(failures)}\n"
                f"Сповіщень: {len(self.notices)}\n\n")
        if failures:
            info += "ПРОГОНИ З ІНШИМ СТАТУСОМ:\n" + "\n".join(
                f"• {run.run_id}: {run.status}" + (f" — {run.failure}" if run.failure else "")
                for run in failures[:80]
            )
            if len(failures) > 80:
                info += f"\n… і ще {len(failures) - 80}"
        self.set_detail(info)
        self.update_memory_view()

    def _build_filter_tree(self):
        self.filters.delete(*self.filters.get_children())
        self.checkbox_map.clear()
        levels = sorted({notice.level for notice in self.notices} | {1, 2, 3})
        for level in levels:
            counts = Counter(n.code for n in self.notices if n.level == level)
            for code in counts:
                self.code_enabled.setdefault((level, code), self.level_enabled.get(level, True))
            selected = all(self.code_enabled[(level, code)] for code in counts) if counts else self.level_enabled.get(level, True)
            partial = bool(counts) and any(self.code_enabled[(level, code)] for code in counts) and not selected
            symbol = "◩" if partial else CHECKED if selected else UNCHECKED
            title = SEVERITY_NAMES.get(level, f"{level} — Інший рівень")
            iid = f"level:{level}"
            self.filters.insert("", "end", iid=iid, text=f"{symbol} {title}", values=(sum(counts.values()),), open=True)
            self.checkbox_map[iid] = (level, None)
            for i, (code, count) in enumerate(sorted(counts.items(), key=lambda entry: (-entry[1], entry[0]))):
                child = f"code:{level}:{i}"
                state = CHECKED if self.code_enabled[(level, code)] else UNCHECKED
                self.filters.insert(iid, "end", iid=child, text=f"{state} {code}", values=(count,))
                self.checkbox_map[child] = (level, code)

    def toggle_filter(self, event):
        iid = self.filters.identify_row(event.y)
        if iid not in self.checkbox_map:
            return
        level, code = self.checkbox_map[iid]
        if code is None:
            codes = {notice.code for notice in self.notices if notice.level == level}
            target = not all(self.code_enabled.get((level, c), True) for c in codes) if codes else not self.level_enabled.get(level, True)
            self.level_enabled[level] = target
            for value in codes:
                self.code_enabled[(level, value)] = target
        else:
            self.code_enabled[(level, code)] = not self.code_enabled.get((level, code), True)
        self._build_filter_tree()
        self.update_results()

    def set_all_filters(self, on: bool):
        for level in self.level_enabled:
            self.level_enabled[level] = on
        for notice in self.notices:
            self.code_enabled[(notice.level, notice.code)] = on
        self._build_filter_tree()
        self.update_results()

    def only_critical(self):
        self.set_all_filters(False)
        self.level_enabled[3] = True
        for notice in self.notices:
            if notice.level == 3:
                self.code_enabled[(notice.level, notice.code)] = True
        self._build_filter_tree()
        self.update_results()

    def update_results(self):
        query = self.find_text.get().casefold().strip()
        self.visible = [notice for notice in self.notices
                        if self.code_enabled.get((notice.level, notice.code), self.level_enabled.get(notice.level, True))
                        and (not query or query in (
                            notice.code + " " + notice.message + " " + notice.recommendation + " " + notice.run_id
                        ).casefold())]
        self.table.delete(*self.table.get_children())
        # Для швидкого відображення показуємо перші 10 000 рядків, експорт містить усі.
        shown = self.visible[:10000]
        for index, notice in enumerate(shown):
            self.table.insert("", "end", iid=str(index),
                              values=(notice.run_id, notice.level, notice.code, notice.timestamp,
                                      notice.thread_id, notice.message.replace("\n", " ")[:280]),
                              tags=(f"level{notice.level}",))
        levels = Counter(n.level for n in self.visible)
        limited = f" (показано перші {len(shown)})" if len(self.visible) > len(shown) else ""
        self.counter.set(f"Сповіщень: {len(self.visible)}{limited}  |  "
                         f"1: {levels[1]}   2: {levels[2]}   3: {levels[3]}"
                         f"  |  усіх прогонів: {len(self.runs)}")

    @staticmethod
    def format_notice(notice: Notice) -> str:
        return (f"Прогін: {notice.run_id}\n"
                f"Рівень: {notice.level} ({SEVERITY_NAMES.get(notice.level, 'інший')})\n"
                f"Код: {notice.code}\n"
                f"Час: {notice.timestamp} с\n"
                f"Потік C++: {notice.thread_id}\n"
                f"Повідомлення: {notice.message}\n"
                f"Рекомендація: {notice.recommendation or '—'}\n"
                f"Файл: {notice.source}\n")

    def show_detail(self):
        indices = self.table.selection()
        if indices:
            idx = number(indices[0], -1)
            if 0 <= idx < len(self.visible):
                self.set_detail(self.format_notice(self.visible[idx]))

    @staticmethod
    def write_text(widget: tk.Text, content: str):
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", content)
        widget.configure(state="disabled")

    def set_detail(self, content: str):
        self.write_text(self.detail, content)

    def copy_text(self, content: str):
        self.clipboard_clear()
        self.clipboard_append(content)
        self.update_idletasks()

    def copy_detail(self):
        self.copy_text(self.detail.get("1.0", "end-1c"))

    def _selected_text(self, all_rows=False):
        indices = (range(len(self.visible)) if all_rows else
                   sorted(number(i) for i in self.table.selection()))
        return ("\n" + "=" * 52 + "\n").join(
            self.format_notice(self.visible[i]) for i in indices if 0 <= i < len(self.visible)
        )

    def copy_rows(self, all_rows=False):
        content = self._selected_text(all_rows)
        if content:
            self.copy_text(content)
        else:
            messagebox.showinfo("Копіювання", "Виберіть хоча б один рядок")

    def export_txt(self):
        if not self.visible:
            messagebox.showinfo("Експорт", "За поточними фільтрами сповіщень немає")
            return
        path = filedialog.asksaveasfilename(title="Зберегти відфільтровані сповіщення",
                                            defaultextension=".txt", filetypes=[("Текст", "*.txt")],
                                            initialfile="debug_notifications.txt")
        if path:
            try:
                Path(path).write_text(self._selected_text(all_rows=True), encoding="utf-8")
            except OSError as exc:
                messagebox.showerror("Помилка запису", str(exc))

    def update_memory_view(self):
        before = self.memory_selected
        self.memory_rows = [run for run in self.selected_runs if run.memory is not None]
        self.memory_table.delete(*self.memory_table.get_children())
        for i, run in enumerate(self.memory_rows):
            stat = run.memory
            assert stat is not None
            self.memory_table.insert("", "end", iid=f"mem:{i}",
                                     values=(run.run_id, run.queue_mode,
                                             run.ovens if run.ovens >= 0 else "—",
                                             memory_size(stat.estimated_peak_bytes),
                                             memory_size(stat.manager_peak_bytes),
                                             memory_size(stat.search_peak_bytes),
                                             clock_time(stat.peak_time_sec),
                                             "Пік за прогін" if stat.kind == "peak" else "Останній пошук"),
                                     tags=("peak" if stat.kind == "peak" else "legacy",))
        real = [run for run in self.memory_rows if run.memory and run.memory.kind == "peak"]
        fallback = [run for run in self.memory_rows if run.memory and run.memory.kind == "last"]
        if real:
            best = max(real, key=lambda run: run.memory.estimated_peak_bytes)
            self.memory_summary.set(
                f"МАКСИМУМ: {memory_size(best.memory.estimated_peak_bytes)} "
                f"({best.memory.estimated_peak_bytes:,} байт) — {best.run_id}"
            )
            self.memory_hint.set(
                f"Історичні піки: {len(real)} із {len(self.selected_runs)} прогонів; "
                f"лише останній пошук: {len(fallback)}. Оцінка приблизна, це не RAM усього процесу. "
                "Один стовпчик або маркер = один прогін, а не посекундний профіль."
            )
        elif fallback:
            best = max(fallback, key=lambda run: run.memory.estimated_peak_bytes)
            self.memory_summary.set(
                f"НАЙБІЛЬША ДОСТУПНА ОЦІНКА: {memory_size(best.memory.estimated_peak_bytes)} — {best.run_id}"
            )
            self.memory_hint.set(
                "У звітах немає queue_memory_peak. Значення взято з QUEUE_MEMORY_USAGE "
                "і стосуються останнього пошуку, а не максимуму за весь прогін."
            )
        else:
            best = None
            self.memory_summary.set("Немає даних про використання пам'яті черги")
            self.memory_hint.set(
                "Увімкніть kReportQueueMemoryPeak = true у main.cpp, "
                "запустіть симулятор і відкрийте нові JSON-звіти."
            )
        paths = {str(run.path) for run in self.memory_rows}
        chosen = before if before in paths else str(best.path) if best else None
        self.memory_selected = chosen
        if chosen:
            for index, run in enumerate(self.memory_rows):
                if str(run.path) == chosen:
                    self.memory_table.selection_set(f"mem:{index}")
                    self.memory_table.see(f"mem:{index}")
                    break
        self.show_memory_detail()
        self.draw_memory_chart()

    def memory_row_selected(self):
        selection = self.memory_table.selection()
        if not selection:
            return
        idx = number(selection[0].removeprefix("mem:"), -1)
        if 0 <= idx < len(self.memory_rows):
            self.memory_selected = str(self.memory_rows[idx].path)
            self.show_memory_detail()
            self.draw_memory_chart()

    def show_memory_detail(self):
        run = next((r for r in self.memory_rows if str(r.path) == self.memory_selected), None)
        if not run or not run.memory:
            self.write_text(self.memory_detail, "Оберіть прогін із доступною статистикою пам'яті.")
            return
        m = run.memory
        kind = "Максимум за весь прогін" if m.kind == "peak" else "Останній пошук (НЕ максимум за прогін)"
        lines = [
            f"Прогін: {run.run_id} | {run.status} | {run.queue_mode} | печей: {run.ovens if run.ovens >= 0 else '—'}",
            f"Джерело: {kind}",
            f"Оцінка загальної пам'яті: {memory_size(m.estimated_peak_bytes)} ({m.estimated_peak_bytes:,} байт)",
            f"Пам'ять менеджера (окремий максимум): {memory_size(m.manager_peak_bytes)}",
            f"Пам'ять пошуку (окремий максимум): {memory_size(m.search_peak_bytes)}",
            f"Час фіксації: {clock_time(m.peak_time_sec)} ({m.peak_time_sec} с)" if m.peak_time_sec >= 0 else "Час фіксації: —",
        ]
        if m.kind == "peak":
            lines.extend([
                f"Операцій пошуку: {m.searches_observed if m.searches_observed >= 0 else '—'}",
                f"Задач у черзі під час піка: {m.queue_tasks_at_peak if m.queue_tasks_at_peak >= 0 else '—'}",
                f"Максимальна кількість задач у черзі: {m.max_queue_tasks if m.max_queue_tasks >= 0 else '—'}",
            ])
        lines.extend([
            f"Розгорнутих вузлів під час оцінки: {m.nodes_at_peak if m.nodes_at_peak >= 0 else '—'}",
            f"Анклавів під час оцінки: {m.enclaves_at_peak if m.enclaves_at_peak >= 0 else '—'}",
            f"Ліміт пошуку в цій операції: {'вичерпано' if m.budget_exhausted_at_peak else 'не вичерпано'}",
            "Окремі максимуми менеджера й пошуку не обов'язково досягнуті одночасно.",
            "Дані приблизні: sizeof/capacity/структури пошуку; не вимірювання RSS процесу.",
            f"Файл: {run.path}",
        ])
        self.write_text(self.memory_detail, "\n".join(lines))

    def draw_memory_chart(self):
        canvas = self.memory_canvas
        canvas.delete("all")
        self.chart_hits.clear()
        width = max(720, canvas.winfo_width())
        height = max(205, canvas.winfo_height())
        data = self.memory_rows
        if not data:
            canvas.configure(scrollregion=(0, 0, width, height))
            canvas.create_text(width / 2, height / 2, text="Немає даних для графіка", fill="#64748b")
            return

        time_mode = self.memory_mode.get() == "Час появи піків"
        if time_mode:
            plot_data = [r for r in data if r.memory and 0 <= r.memory.peak_time_sec <= 86400]
        else:
            plot_data = data
        if not plot_data:
            canvas.configure(scrollregion=(0, 0, width, height))
            canvas.create_text(width / 2, height / 2,
                               text="У вибраних звітах відсутній час фіксації піка", fill="#64748b")
            return
        full_width = max(width, 140 + len(plot_data) * 74) if not time_mode else max(width, 1050)
        canvas.configure(scrollregion=(0, 0, full_width, height))
        left, right, top, bottom = 78, full_width - 36, 38, height - 60
        plot_h = max(1, bottom - top)
        y_max = max(1, max(r.memory.estimated_peak_bytes for r in plot_data)) * 1.16
        max_peak = max((r.memory.estimated_peak_bytes for r in plot_data
                        if r.memory.kind == "peak"), default=None)

        for tick in range(5):
            value = y_max * tick / 4
            y = bottom - plot_h * tick / 4
            canvas.create_line(left, y, right, y, fill="#e2e8f0", dash=(3, 4) if tick else ())
            canvas.create_text(left - 9, y, text=memory_size(int(value)), anchor="e", fill="#64748b", font=("Segoe UI", 9))
        canvas.create_line(left, top, left, bottom, fill="#475569")
        canvas.create_line(left, bottom, right, bottom, fill="#475569")
        canvas.create_text(left, 15, text="Оціночна пам'ять черги", anchor="w", fill="#334155",
                           font=("Segoe UI", 10, "bold"))
        canvas.create_rectangle(right - 365, 9, right - 350, 22, fill="#0f766e", outline="")
        canvas.create_text(right - 344, 16, text="Пік за прогін", anchor="w", fill="#334155", font=("Segoe UI", 9))
        canvas.create_rectangle(right - 245, 9, right - 230, 22, fill="#a98b56", outline="")
        canvas.create_text(right - 224, 16, text="Лише останній пошук", anchor="w", fill="#334155", font=("Segoe UI", 9))

        if max_peak is not None:
            y_max_line = bottom - max_peak / y_max * plot_h
            canvas.create_line(left, y_max_line, right, y_max_line, fill="#b91c1c", dash=(6, 4), width=1)
            canvas.create_text(left + 8, max(top + 5, y_max_line - 8), text=f"Макс. {memory_size(max_peak)}",
                               anchor="sw", fill="#b91c1c", font=("Segoe UI", 9, "bold"))

        selected = self.memory_selected
        for i, run in enumerate(plot_data):
            m = run.memory
            if not m:
                continue
            value = m.estimated_peak_bytes
            y = bottom - value / y_max * plot_h
            color = "#0f766e" if m.kind == "peak" else "#a98b56"
            is_selected = str(run.path) == selected
            is_best = m.kind == "peak" and value == max_peak
            if time_mode:
                x = left + (m.peak_time_sec / 86400) * (right - left)
                r = 6 if is_selected or is_best else 4
                canvas.create_oval(x - r, y - r, x + r, y + r, fill=color,
                                   outline="#b91c1c" if is_best else "#0f172a" if is_selected else "white",
                                   width=3 if is_best or is_selected else 1)
                self.chart_hits.append((x - 10, y - 12, x + 10, y + 12, str(run.path)))
            else:
                segment = (right - left) / max(1, len(plot_data))
                x = left + segment * (i + 0.5)
                half = min(24, segment * 0.35)
                canvas.create_rectangle(x - half, y, x + half, bottom,
                                        fill=color, outline="#b91c1c" if is_best else
                                        "#1e293b" if is_selected else "", width=3 if is_best else 2 if is_selected else 0)
                self.chart_hits.append((x - half, y - 5, x + half, bottom, str(run.path)))
                label = f"{run.ovens} печ." if run.ovens >= 0 else run.run_id[:10]
                canvas.create_text(x, bottom + 16, text=label, fill="#334155", font=("Segoe UI", 9))
                if len(plot_data) <= 14:
                    canvas.create_text(x, max(top + 2, y - 8), text=memory_size(value), anchor="s",
                                       fill="#0f172a", font=("Segoe UI", 8))
        if time_mode:
            for hour in (0, 6, 12, 18, 24):
                x = left + (hour / 24) * (right - left)
                canvas.create_line(x, bottom, x, bottom + 5, fill="#475569")
                canvas.create_text(x, bottom + 17, text=f"{hour:02d}:00", fill="#334155", font=("Segoe UI", 9))
            canvas.create_text((left + right) / 2, height - 12, text="Час симуляції, коли зафіксовано пік",
                               fill="#64748b", font=("Segoe UI", 9))
        else:
            canvas.create_text((left + right) / 2, height - 12, text="Прогони (не безперервний часовий ряд)",
                               fill="#64748b", font=("Segoe UI", 9))

    def _hit_run(self, event):
        x = self.memory_canvas.canvasx(event.x)
        y = self.memory_canvas.canvasy(event.y)
        for x1, y1, x2, y2, path in reversed(self.chart_hits):
            if x1 <= x <= x2 and y1 <= y <= y2:
                return next((run for run in self.memory_rows if str(run.path) == path), None)
        return None

    def chart_motion(self, event):
        run = self._hit_run(event)
        if run and run.memory:
            source = "пік за прогін" if run.memory.kind == "peak" else "останній пошук"
            self.memory_chart_status.set(
                f"{run.run_id}: {memory_size(run.memory.estimated_peak_bytes)} | "
                f"{clock_time(run.memory.peak_time_sec)} | {source}. Натисніть для подробиць."
            )
        else:
            self.memory_chart_status.set("")

    def chart_click(self, event):
        run = self._hit_run(event)
        if not run:
            return
        self.memory_selected = str(run.path)
        for i, item in enumerate(self.memory_rows):
            if str(item.path) == self.memory_selected:
                iid = f"mem:{i}"
                self.memory_table.selection_set(iid)
                self.memory_table.see(iid)
                break
        self.show_memory_detail()
        self.draw_memory_chart()

    def _maximum_run(self):
        peaks = [run for run in self.memory_rows if run.memory and run.memory.kind == "peak"]
        candidates = peaks or self.memory_rows
        return max(candidates, key=lambda run: run.memory.estimated_peak_bytes) if candidates else None

    def copy_maximum(self):
        run = self._maximum_run()
        if not run or not run.memory:
            messagebox.showinfo("Пам'ять", "Немає вимірювань для копіювання")
            return
        kind = "історичний пік" if run.memory.kind == "peak" else "лише останній пошук"
        self.copy_text(f"{run.run_id}: {memory_size(run.memory.estimated_peak_bytes)} "
                       f"({run.memory.estimated_peak_bytes} байт); {kind}")

    def export_memory_csv(self):
        if not self.memory_rows:
            messagebox.showinfo("Експорт", "Немає статистики пам'яті для експорту")
            return
        path = filedialog.asksaveasfilename(title="Експортувати статистику пам'яті",
                                            defaultextension=".csv", filetypes=[("CSV", "*.csv")],
                                            initialfile="queue_memory_peaks.csv")
        if not path:
            return
        try:
            with open(path, "w", newline="", encoding="utf-8-sig") as stream:
                writer = csv.writer(stream, delimiter=";")
                writer.writerow(["run_id", "queue_mode", "ovens", "source", "estimated_peak_bytes",
                                 "manager_peak_bytes", "search_peak_bytes", "searches_observed",
                                 "peak_time_sec", "queue_tasks_at_peak", "max_queue_tasks",
                                 "nodes_at_peak", "enclaves_at_peak", "budget_exhausted_at_peak", "file"])
                for run in self.memory_rows:
                    m = run.memory
                    assert m is not None
                    writer.writerow([run.run_id, run.queue_mode, run.ovens, m.kind,
                                     m.estimated_peak_bytes, m.manager_peak_bytes, m.search_peak_bytes,
                                     m.searches_observed, m.peak_time_sec, m.queue_tasks_at_peak,
                                     m.max_queue_tasks, m.nodes_at_peak, m.enclaves_at_peak,
                                     int(m.budget_exhausted_at_peak), str(run.path)])
        except OSError as exc:
            messagebox.showerror("Помилка експорту", str(exc))


def main():
    location = Path(sys.argv[1]) if len(sys.argv) > 1 else detect_folder(Path(__file__).resolve())
    app = DebugViewer(location)
    app.mainloop()


if __name__ == "__main__":
    main()
